#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <glad/glad.h>
#include <glad/glad_egl.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <cflogprinter.h>

typedef unsigned int uint;

struct wl_state {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_list outputs;  // struct display_output::link
    char *monitor; // User selected output
    int surface_layer;
};

struct display_output {
    uint32_t wl_name;
    struct wl_output *wl_output;
    char *name;
    char *identifier;

    struct wl_state *state;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_egl_window *egl_window;
    EGLSurface *egl_surface;

    uint32_t width, height;
    uint32_t scale;

    struct wl_list link;

    struct wl_callback *frame_callback;
    bool redraw_needed;
};

static EGLConfig egl_config;
static EGLDisplay *egl_display;
static EGLContext *egl_context;

static mpv_handle *mpv;
static mpv_render_context *mpv_glcontext;
static int wakeup_pipe[2];
static char *video_path;
static char *mpv_options;

static struct {
    char **pauselist;
    char **stoplist;

    int argc;
    char **argv_copy;
    char *save_info;

    bool auto_pause;
    bool auto_stop;

    int is_paused;
    bool frame_ready;
    bool stop_render_loop;

} halt_info = {NULL, NULL, 0, NULL, NULL, 0, 0, 0, 0, 0};

static pthread_t threads[5] = {0};

static uint SLIDESHOW_TIME = 0;
static bool SHOW_OUTPUTS = false;
static int VERBOSE = 0;

static void nop() {}

static void exit_cleanup() {

    // Give mpv a chance to finish
    halt_info.stop_render_loop = 1;
    for (int trys=10; halt_info.stop_render_loop && trys > 0; trys--) {
        usleep(10000);
    }
    // If render loop failed to stop it's self
    if (halt_info.stop_render_loop && VERBOSE)
        cflp_warning("Failed to quit mpv");
    

    // Cancel all threads
    for (uint i=0; threads[i] != 0; i++) {
        if (pthread_self() != threads[i])
            pthread_cancel(threads[i]);
    }

    if (mpv_glcontext)
       mpv_render_context_free(mpv_glcontext);
    if (mpv)
        mpv_terminate_destroy(mpv);

    if (egl_context)
        eglDestroyContext(egl_display, egl_context);
}

static void exit_mpvpaper(int reason) {
    if (VERBOSE)
        cflp_info("Exiting mpvpaper");
    exit_cleanup();
    exit(reason);
}

static void *exit_by_pthread() { exit_mpvpaper(EXIT_SUCCESS); pthread_exit(NULL);}

static void handle_signal(int signum) {
    (void)signum;
    // Separate thread to avoid crash
    pthread_t thread;
    pthread_create(&thread, NULL, exit_by_pthread, NULL);
}

const static struct wl_callback_listener wl_surface_frame_listener;

static void render(struct display_output *output) {
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &(mpv_opengl_fbo){
            .fbo = 0,
            .w = output->width * output->scale,
            .h = output->height * output->scale,
        }},
        // Flip rendering (needed due to flipped GL coordinate system).
        {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
        // Do not wait for a fresh frame to render
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &(int){0}},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };

    if (!eglMakeCurrent(egl_display, output->egl_surface, output->egl_surface, egl_context))
        cflp_error("Failed to make output surface current 0x%X", eglGetError());

    glViewport(0, 0, output->width * output->scale, output->height * output->scale);

    // Render frame
    int mpv_err = mpv_render_context_render(mpv_glcontext, render_params);
    if (mpv_err < 0)
        cflp_error("Failed to render frame with mpv, %s", mpv_error_string(mpv_err));

    // Callback new frame
    output->frame_callback = wl_surface_frame(output->surface);
    wl_callback_add_listener(output->frame_callback, &wl_surface_frame_listener, output);
    output->redraw_needed = false;

    // Display frame
    if (!eglSwapBuffers(egl_display, output->egl_surface))
        cflp_error("Failed to swap egl buffers 0x%X", eglGetError());
}

static void frame_handle_done(void *data, struct wl_callback *callback, uint32_t frame_time) {
    (void)frame_time;
    struct display_output *output = data;
    wl_callback_destroy(callback);

    // Display is ready for new frame
    output->frame_callback = NULL;

    // Reset deadman switch timer
    halt_info.frame_ready = 1;

    // Render next frame
    if (output->redraw_needed) {
        if (VERBOSE == 2)
            cflp_info("%s is ready for MPV to render the next frame", output->name);
        render(output);
    }
}

const static struct wl_callback_listener wl_surface_frame_listener = {
    .done = frame_handle_done,
};

static void stop_mpvpaper() {

    // Save video position to arg -Z
    const char *time_pos = mpv_get_property_string(mpv, "time-pos");
    const char *playlist_pos = mpv_get_property_string(mpv, "playlist-pos");

    char save_info[30];
    snprintf(save_info, sizeof(save_info), "%s %s", time_pos, playlist_pos);

    char **new_argv = calloc(halt_info.argc+3, sizeof(char*)); // Plus 3 for adding in -Z

    int i;
    for (i = 0; i < halt_info.argc; i++) {
        new_argv[i] = strdup(halt_info.argv_copy[i]);
    }
    new_argv[i] = strdup("-Z");
    new_argv[i+1] = strdup(save_info);
    new_argv[i+2] = NULL;

    // Get the "real" cwd
    char exe_dir[1024];
    int cut_point = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir));
    for (uint i=cut_point; i > 1; i--) {
        if (exe_dir[i] == '/') {
            exe_dir[i+1] = '\0';
            break;
        }
    }

    exit_cleanup();

    // Start holder script
    execv(strcat(exe_dir, "mpvpaper-holder"), new_argv);

    cflp_error("Failed to stop mpvpaper");
    exit(EXIT_FAILURE);
}

// Allow pthread_cancel while sleeping
static void pthread_sleep(uint time) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    sleep(time);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
}
static void pthread_usleep(uint time) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    usleep(time);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
}

static char *check_watch_list(char **list) {

    char pid_name[512] = {0};

    for (uint i=0; list[i] != NULL; i++) {
        strcpy(pid_name, "pidof ");
        strcat(pid_name, list[i]);
        strcat(pid_name, " > /dev/null");

        // Stop if program is open
        if (!system(pid_name))
            return list[i];
    }
    return NULL;
}

static void *monitor_pauselist() {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    bool list_paused = 0;

    while (halt_info.pauselist) {

        char *app = check_watch_list(halt_info.pauselist);
        if (app && !list_paused && !halt_info.is_paused) {
            if (VERBOSE)
                cflp_info("Pausing for %s", app);
            mpv_command_async(mpv, 0, (const char*[]) {"set", "pause", "yes", NULL});
            list_paused = 1;
            halt_info.is_paused += 1;
        } else if (!app && list_paused) {
            list_paused = 0;
            if (halt_info.is_paused)
                halt_info.is_paused -= 1;
        }

        pthread_sleep(1);
    }
    pthread_exit(NULL);
}

static void *monitor_stoplist() {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    while (halt_info.stoplist) {

        char *app = check_watch_list(halt_info.stoplist);
        if (app) {
            if (VERBOSE)
                cflp_info("Stopping for %s", app);
            stop_mpvpaper();
        }

        pthread_sleep(1);
    }
    pthread_exit(NULL);
}

static void *handle_auto_pause() {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    while (halt_info.auto_pause) {

        // Set deadman switch timer
        halt_info.frame_ready = 0;
        pthread_sleep(2);
        if (!halt_info.frame_ready && !halt_info.is_paused) {
            if (VERBOSE)
                    cflp_info("Pausing because mpvpaper is hidden");
            mpv_command_async(mpv, 0, (const char*[]) {"set", "pause", "yes", NULL});
            halt_info.is_paused += 1;

            while(!halt_info.frame_ready) {
                pthread_usleep(10000);
            }
            if (halt_info.is_paused)
                halt_info.is_paused -= 1;
        }
    }
    pthread_exit(NULL);
}

static void *handle_auto_stop() {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    while (halt_info.auto_stop) {

        // Set deadman switch timer
        halt_info.frame_ready = 0;
        pthread_sleep(2);
        if (!halt_info.frame_ready) {
            if (VERBOSE)
                    cflp_info("Stopping because mpvpaper is hidden");
            stop_mpvpaper();
        }
    }
    pthread_exit(NULL);
}

static void *handle_mpv_events() {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    int mpv_paused = 0;
    time_t start_time = time(NULL);

    const int MPV_OBSERVE_PAUSE = 1;
    mpv_observe_property(mpv, MPV_OBSERVE_PAUSE, "pause", MPV_FORMAT_FLAG);

    while (true) {
        if (SLIDESHOW_TIME) {
            if ((time(NULL) - start_time) >= SLIDESHOW_TIME) {
                mpv_command_async(mpv, 0, (const char*[]) {"playlist-next", NULL});
                start_time = time(NULL);
            }
        }

        mpv_event *event = mpv_wait_event(mpv, 0);

        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            exit_mpvpaper(EXIT_SUCCESS);
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            if (event->reply_userdata == MPV_OBSERVE_PAUSE) {
                mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &mpv_paused);
                if (mpv_paused) {
                    // User paused
                    if (!halt_info.is_paused)
                        halt_info.is_paused += 1;
                } else {
                    halt_info.is_paused = 0;
                }
            }
        }

        if (!halt_info.is_paused && mpv_paused)
            mpv_command_async(mpv, 0, (const char*[]) {"set", "pause", "no", NULL});

        pthread_usleep(10000);
    }

    mpv_unobserve_property(mpv, MPV_OBSERVE_PAUSE);

    pthread_exit(NULL);
}

static void init_threads() {
    uint id = 0;

    pthread_create(&threads[id], NULL, handle_mpv_events, NULL);
    id++;

    // Thread for monitoring if mpvpaper is hidden
    if (halt_info.auto_pause) {
        pthread_create(&threads[id], NULL, handle_auto_pause, NULL);
        id++;
    } else if (halt_info.auto_stop) {
        pthread_create(&threads[id], NULL, handle_auto_stop, NULL);
        id++;
    }

    // Threads for monitoring watch lists
    if (halt_info.pauselist) {
        pthread_create(&threads[id], NULL, monitor_pauselist, NULL);
        id++;
    }
    if (halt_info.stoplist) {
        pthread_create(&threads[id], NULL, monitor_stoplist, NULL);
        id++;
    }
}

static void set_init_mpv_options(const struct wl_state *state) {
    // Enable user control through terminal by default
    mpv_set_option_string(mpv, "input-default-bindings", "yes");
    mpv_set_option_string(mpv, "input-terminal", "yes");
    mpv_set_option_string(mpv, "terminal", "yes");

    // Load user configs
    const char *home_dir = getenv("HOME");
    char *config_path = calloc(strlen(home_dir)+1 + 30, sizeof(char));
    strcpy(config_path, home_dir);

    char loaded_configs[50] = "";

    strcpy(config_path+strlen(home_dir), "/.config/mpv/mpv.conf");
    if (mpv_load_config_file(mpv, config_path) == 0)
        strcat(loaded_configs, "mpv.conf ");
    strcpy(config_path+strlen(home_dir), "/.config/mpv/input.conf");
    if (mpv_load_config_file(mpv, config_path) == 0)
        strcat(loaded_configs, "input.conf ");
    strcpy(config_path+strlen(home_dir), "/.config/mpv/fonts.conf");
    if (mpv_load_config_file(mpv, config_path) == 0)
        strcat(loaded_configs, "fonts.conf ");
    free(config_path);

    if (VERBOSE && strcmp(loaded_configs, ""))
        cflp_info("Loaded [ %s] user configs from \"~/.config/mpv/\"", loaded_configs);

    // Convenience options passed for slideshow mode
    if (SLIDESHOW_TIME != 0) {
        mpv_set_option_string(mpv, "loop", "yes");
        mpv_set_option_string(mpv, "loop-playlist", "yes");
    }

    // Set mpv_options passed
    if (mpv_options) {
        // Create config file name
        char *opt_config_path = calloc(strlen("/tmp/mpvpaper.config")+1 + strlen(state->monitor)+1, sizeof(char));
        strcpy(opt_config_path, "/tmp/mpvpaper");
        strcat(opt_config_path, state->monitor);
        strcat(opt_config_path, ".config");
        // Put options into config file
        FILE *file = fopen(opt_config_path, "w");
        fputs(mpv_options, file);
        fclose(file);

        mpv_load_config_file(mpv, opt_config_path);
        remove(opt_config_path);
        free(opt_config_path);
    }
}

static void *get_proc_address_mpv(void *ctx, const char *name) {
    (void)ctx;
    return eglGetProcAddress(name);
}

static void render_update_callback(void *callback_ctx) {
    (void)callback_ctx;
    uint8_t tmp = 0;

    if (write(wakeup_pipe[1], &tmp, 1) == -1)
        exit_mpvpaper(EXIT_FAILURE);
}

static void init_mpv(const struct wl_state *state) {
    int mpv_err;

    mpv = mpv_create();
    if (!mpv) {
        cflp_error("Failed creating mpv context");
        exit_mpvpaper(EXIT_FAILURE);
    }

    set_init_mpv_options(state);

    mpv_err = mpv_initialize(mpv);
    if (mpv_err < 0) {
        cflp_error("Failed to init mpv, %s", mpv_error_string(mpv_err));
        exit_mpvpaper(EXIT_FAILURE);
    }

    // Force libmpv vo as nothing else will work
    char *vo_option = mpv_get_property_string(mpv, "options/vo");
    if (strcmp(vo_option, "libmpv") != 0 && strcmp(vo_option, "") != 0) {
        cflp_warning("mpvpaper does not support any other vo than \"libmpv\"");
        mpv_set_option_string(mpv, "vo", "libmpv");
    }

    // Have mpv render onto egl context
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_WL_DISPLAY, state->display},
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &(mpv_opengl_init_params){
            .get_proc_address = get_proc_address_mpv,
        }},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    mpv_err = mpv_render_context_create(&mpv_glcontext, mpv, params);
    if (mpv_err < 0) {
        cflp_error("Failed to initialize mpv GL context, %s", mpv_error_string(mpv_err));
        exit_mpvpaper(EXIT_FAILURE);
    }

    // Restore video position after auto stop event
    char *default_start = NULL;
    if (halt_info.save_info) {

        char time_pos[10];
        char playlist_pos[10];
        sscanf(halt_info.save_info, "%s %s", time_pos, playlist_pos);

        if (VERBOSE)
            cflp_info("Restoring previous time: %s and playlist position: %s", time_pos, playlist_pos);
        // Save default start pos
        default_start = mpv_get_property_string(mpv, "start");
        // Restore video position
        mpv_command(mpv, (const char*[]) {"set", "start", time_pos, NULL});
        // Recover playlist pos, that is if it's not shuffled...
        mpv_command(mpv, (const char*[]) {"set", "playlist-start", playlist_pos, NULL});
    }

    mpv_command(mpv, (const char*[]) {"loadfile", video_path, NULL});

    mpv_event *event = mpv_wait_event(mpv, 1);
    while (event->event_id != MPV_EVENT_FILE_LOADED) {
        event = mpv_wait_event(mpv, 1);
    }
    if (VERBOSE)
        cflp_info("Loaded %s", video_path);

    // Return start pos to default
    if (default_start)
        mpv_command(mpv, (const char*[]) {"set", "start", default_start, NULL});

    // mpv must never idle
    mpv_command(mpv, (const char*[]) {"set", "idle", "no", NULL});

    mpv_render_context_set_update_callback(mpv_glcontext, render_update_callback, NULL);
}

static void init_egl(struct wl_state *state) {
    egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, state->display, NULL);
    if (egl_display == EGL_NO_DISPLAY) {
        cflp_error("Failed to get EGL display");
        exit_mpvpaper(EXIT_FAILURE);
    }
    if (!eglInitialize(egl_display, NULL, NULL)) {
        cflp_error("Failed to initialize EGL 0x%X", eglGetError());
        exit_mpvpaper(EXIT_FAILURE);
    }

    eglBindAPI(EGL_OPENGL_API);
    const EGLint win_attrib[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE
    };

    EGLint  num_config;
    if (!eglChooseConfig(egl_display, win_attrib, &egl_config, 1, &num_config)) {
        cflp_error("Failed to set EGL frame buffer config 0x%X", eglGetError());
        exit_mpvpaper(EXIT_FAILURE);
    }

    // Check for OpenGL compatibility for creating egl context
    static const struct { int major, minor; } gl_versions[] = {
        {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0},
        {3, 3}, {3, 2}, {3, 1}, {3, 0},
        {0, 0}
    };
    egl_context = NULL;
    for (uint i = 0; gl_versions[i].major > 0; i++) {
        const EGLint ctx_attrib[] = {
            EGL_CONTEXT_MAJOR_VERSION, gl_versions[i].major,
            EGL_CONTEXT_MINOR_VERSION, gl_versions[i].minor,
            EGL_NONE
        };
        egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx_attrib);
        if (egl_context) {
            if (VERBOSE)
                cflp_info("OpenGL %i.%i EGL context created", gl_versions[i].major, gl_versions[i].minor);
            break;
        }
    }
    if (!egl_context) {
        cflp_error("Failed to create EGL context 0x%X", eglGetError());
        exit_mpvpaper(EXIT_FAILURE);
    }

    if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context)) {
        cflp_error("Failed to make context current 0x%X", eglGetError());
        exit_mpvpaper(EXIT_FAILURE);
    }

    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
        cflp_error("Failed to load OpenGL 0x%X", eglGetError());
        exit_mpvpaper(EXIT_FAILURE);
    }
}

static void destroy_display_output(struct display_output *output) {
    if (!output)
        return;
    wl_list_remove(&output->link);
    if (output->layer_surface != NULL)
        zwlr_layer_surface_v1_destroy(output->layer_surface);
    if (output->surface != NULL)
        wl_surface_destroy(output->surface);
    if (output->egl_surface)
        eglDestroySurface(egl_display, output->egl_surface);
    if (output->egl_window)
        wl_egl_window_destroy(output->egl_window);
    wl_output_destroy(output->wl_output);

    free(output->name);
    free(output->identifier);
    free(output);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial,
        uint32_t width, uint32_t height) {

    struct display_output *output = data;
    output->width = width;
    output->height = height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    wl_surface_set_buffer_scale(output->surface, output->scale);

    if (!output->egl_window) {
        output->egl_window = wl_egl_window_create(output->surface, output->width * output->scale, output->height * output->scale);
        output->egl_surface = eglCreatePlatformWindowSurface(egl_display, egl_config, output->egl_window, NULL);
        if (!output->egl_surface) {
            cflp_error("Failed to create EGL surface for %s 0x%X", output->name, eglGetError());
            destroy_display_output(output);
            return;
        }

        if (!eglMakeCurrent(egl_display, output->egl_surface, output->egl_surface, egl_context))
            cflp_error("Failed to make output surface current 0x%X", eglGetError());
        eglSwapInterval(egl_display, 0);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

        // Start render loop
        render(output);
    } else {
        wl_egl_window_resize(output->egl_window, output->width * output->scale, output->height * output->scale, 0, 0);
    }
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)surface;

    struct display_output *output = data;
    if (VERBOSE)
        cflp_info("Destroying output %s (%s)", output->name, output->identifier);
    destroy_display_output(output);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void create_layer_surface(struct display_output *output) {
    output->surface = wl_compositor_create_surface(output->state->compositor);

    // Empty input region
    struct wl_region *input_region = wl_compositor_create_region(output->state->compositor);
    wl_surface_set_input_region(output->surface, input_region);
    wl_region_destroy(input_region);

    output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        output->state->layer_shell, output->surface, output->wl_output, output->state->surface_layer, "mpvpaper");

    zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(output->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(output->layer_surface, &layer_surface_listener, output);
    wl_surface_commit(output->surface);
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)wl_output;

    struct display_output *output = data;
    output->name = strdup(name);
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)wl_output;

    struct display_output *output = data;

    bool name_ok = (strstr(output->state->monitor, output->name) != NULL) || (strcmp(output->state->monitor, "*") == 0);
    if (name_ok && !output->layer_surface) {
        if (VERBOSE)
            cflp_info("Output %s (%s) selected", output->name, output->identifier);
        create_layer_surface(output);
    }
    if (!name_ok) {
        if (SHOW_OUTPUTS)
            cflp_info("Output %s (%s) found", output->name, output->identifier);
        destroy_display_output(output);
    }
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t scale) {
    struct display_output *output = data;
    output->scale = scale;
}

static void output_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)wl_output;

    struct display_output *output = data;

    // wlroots currently sets the description to `make model serial (name)`
    // If this changes in the future, this will need to be modified.
    char *paren = strrchr(description, '(');
    if (paren) {
        size_t length = paren - description;
        output->identifier = calloc(length, sizeof(char));
        if (!output->identifier) {
            cflp_warning("Failed to allocate output identifier");
            return;
        }
        strncpy(output->identifier, description, length);
        output->identifier[length - 1] = '\0';
    }
}

static const struct wl_output_listener output_listener = {
    .geometry = nop,
	.mode = nop,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)version;

    struct wl_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct display_output *output = calloc(1, sizeof(struct display_output));
        output->scale = 1; // Default to no scaling
        output->state = state;
        output->wl_name = name;
        output->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 4);
        wl_output_add_listener(output->wl_output, &output_listener, output);
        wl_list_insert(&state->outputs, &output->link);

    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;

    struct wl_state *state = data;
    struct display_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &state->outputs, link) {
        if (output->wl_name == name) {
            cflp_info("Destroying output %s (%s)", output->name, output->identifier);
            destroy_display_output(output);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static char **get_watch_list(char *path_name) {

    FILE *file = fopen(path_name, "r");
    if (file) {
        // Get alloc size
        fseek(file, 0L, SEEK_END);
        char **list = calloc(ftell(file) + 1, sizeof(char));
        rewind(file);

        // Read lines
        char app[512];
        for (uint i=0; fscanf(file, "%s", app) == 1; i++) {
            list[i] = strdup(app);
        }

        fclose(file);
        // If any app found
        if (list[0])
            return list;
    }
    return NULL;
}

static void copy_argv(int argc, char* argv[]) {
    halt_info.argc = argc;
    halt_info.argv_copy = calloc(argc+1, sizeof(char*));

    int j = 0;
    for (int i=0; i < argc; i++) {
        if (strcmp(argv[i], "-Z") == 0) { // Remove hidden opt
            i++; // Skip optind
            halt_info.argc -= 2;
        } else {
            halt_info.argv_copy[j] = strdup(argv[i]);
            j++;
        }
    }
}

static void set_watch_lists() {
    const char *home_dir = getenv("HOME");

    char *pause_path = calloc(strlen(home_dir)+1 + strlen("/.config/mpvpaper/pauselist")+1, sizeof(char));
    strcpy(pause_path, home_dir);
    strcat(pause_path, "/.config/mpvpaper/pauselist");
    halt_info.pauselist = get_watch_list(pause_path);
    free(pause_path);

    char *stop_path = calloc(strlen(home_dir)+1 + strlen("/.config/mpvpaper/stoplist")+1, sizeof(char));
    strcpy(stop_path, home_dir);
    strcat(stop_path, "/.config/mpvpaper/stoplist");
    halt_info.stoplist = get_watch_list(stop_path);
    free(stop_path);

    if (VERBOSE && halt_info.pauselist)
        cflp_info("pauselist found and will be monitored");
    if (VERBOSE && halt_info.stoplist)
        cflp_info("stoplist found and will be monitored");
}

static void parse_command_line(int argc, char **argv, struct wl_state *state) {

    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"help-output", no_argument, NULL, 'd'},
        {"verbose", no_argument, NULL, 'v'},
        {"fork", no_argument, NULL, 'f'},
        {"auto-pause", no_argument, NULL, 'p'},
        {"auto-stop", no_argument, NULL, 's'},
        {"slideshow", required_argument, NULL, 'n'},
        {"layer", required_argument, NULL, 'l'},
        {"mpv-options", required_argument, NULL, 'o'},
        {0, 0, 0, 0}};

    const char *usage =
        "Usage: mpvpaper [options] <output> <url|path filename>\n"
        "\n"
        "Example: mpvpaper -vs -o \"no-audio loop\" DP-2 /path/to/video\n"
        "\n"
        "Options:\n"
        "--help         -h              Displays this help message\n"
        "--help-output  -d              Displays all available outputs and quits\n"
        "--verbose      -v              Be more verbose (-vv for higher verbosity)\n"
        "--fork         -f              Forks mpvpaper so you can close the terminal\n"
        "--auto-pause   -p              Automagically* pause mpv when the wallpaper is hidden\n"
        "                               This saves CPU usage, more or less, seamlessly\n"
        "--auto-stop    -s              Automagically* stop mpv when the wallpaper is hidden\n"
        "                               This saves CPU/RAM usage, although more abruptly\n"
        "--slideshow    -n SECS         Slideshow mode plays the next video in a playlist every ? seconds\n"
        "                               And passes mpv options \"loop loop-playlist\" for convenience\n"
        "--layer        -l LAYER        Specifies shell surface layer to run on (background by default)\n"
        "--mpv-options  -o \"OPTIONS\"    Forwards mpv options (Must be within quotes\"\")\n"
        "\n"
        "* See man page for more details\n";

    char *layer_name;

    int opt;
    while ((opt = getopt_long(argc, argv, "hdvfpsn:l:o:Z:", long_options, NULL)) != -1) {

        switch (opt) {
            case 'h':
                fprintf(stdout, "%s", usage);
                exit(EXIT_SUCCESS);
            case 'd':
                SHOW_OUTPUTS = true;
                state->monitor = "";
                return;
            case 'v':
                VERBOSE += 1;
                break;
            case 'f':
                if (fork() > 0)
                    exit(EXIT_SUCCESS);

                fclose(stdout);
                fclose(stderr);
                fclose(stdin);
                break;
            case 'p':
                halt_info.auto_pause = 1;
                halt_info.auto_stop = 0;
                break;
            case 's':
                halt_info.auto_stop = 1;
                halt_info.auto_pause = 0;
                break;
            case 'n':
                SLIDESHOW_TIME = atoi(optarg);
                if (SLIDESHOW_TIME == 0) 
                    cflp_warning(
                        "0 or invalid time set for slideshow\n"
                        "Please use a positive integer");
                break;
            case 'l':
                layer_name = strdup(optarg);
                if (layer_name == NULL) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
                } else if (strcasecmp(layer_name, "top") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
                } else if (strcasecmp(layer_name, "bottom") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
                } else if (strcasecmp(layer_name, "background") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
                } else if (strcasecmp(layer_name, "overlay") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
                } else {
                    cflp_error(
                        "%s is not a shell surface layer\n"
                        "Your options are: top, bottom, background and overlay", layer_name);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'o':
                mpv_options = strdup(optarg);
                // Replace spaces with newlines
                for (int i = 0; i < (int)strlen(mpv_options); i++) {
                    if (mpv_options[i] == ' ')
                        mpv_options[i] = '\n';
                }
                break;

            case 'Z': // Hidden option to recover video pos after stopping
                halt_info.save_info = strdup(optarg);
                break;
        }
    }

    if (VERBOSE)
        cflp_info("Verbose Level %i enabled", VERBOSE);

    // Need at least a display and video
    if (optind + 1 >= argc) {
        cflp_error("Not enough args passed");
        fprintf(stderr, "%s", usage);
        exit(EXIT_FAILURE);
    }

    state->monitor = strdup(argv[optind]);
    video_path = strdup(argv[optind+1]);
}

static void check_paper_processes() {
    // Check for other wallpaper process running
    const char *other_wallpapers[] = {"swaybg", "glpaper", "hyprpaper", "wpaperd"};
    char wallpaper_sbuffer[50] = {0};

    for (int i=0; i < sizeof(other_wallpapers) / sizeof(other_wallpapers[0]); i++) {

        strcpy(wallpaper_sbuffer, "pidof ");
        strcat(wallpaper_sbuffer, other_wallpapers[i]);
        strcat(wallpaper_sbuffer, " > /dev/null");

        if (!system(wallpaper_sbuffer))
            cflp_warning("%s is running. This may block mpvpaper from being seen.", other_wallpapers[i]);
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, handle_signal);
    signal(SIGQUIT, handle_signal);
    signal(SIGTERM, handle_signal);

    check_paper_processes();

    struct wl_state state = {0};
    wl_list_init(&state.outputs);

    parse_command_line(argc, argv, &state);
    set_watch_lists();
    if (halt_info.auto_stop || halt_info.stoplist)
        copy_argv(argc, argv);

    // Create pipe for checking render_update_callback()
    if (pipe(wakeup_pipe) == -1) {
        cflp_error("Creating a self-pipe failed.");
        return EXIT_FAILURE;
    }
    fcntl(wakeup_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(wakeup_pipe[1], F_SETFD, FD_CLOEXEC);

    // Connect to Wayland compositor
    state.display = wl_display_connect(NULL);
    if (!state.display) {
        cflp_error(
            "Unable to connect to the compositor.\n"
            "If your compositor is running, check or set the WAYLAND_DISPLAY environment variable.");
        return EXIT_FAILURE;
    }
    if (VERBOSE)
        cflp_success("Connected to Wayland compositor");

    // Don't start egl and mpv if just displaying outputs
    if (!SHOW_OUTPUTS) {
        // Init render before outputs
        init_egl(&state);
        if (VERBOSE)
            cflp_success("EGL initialized");
        init_mpv(&state);
        init_threads();
        if (VERBOSE)
            cflp_success("MPV initialized");
    }

    // Setup wayland surfaces
    struct wl_registry *registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);
    if (state.compositor == NULL || state.layer_shell == NULL) {
        cflp_error("Missing a required Wayland interface");
        return EXIT_FAILURE;
    }

    // Check outputs
    wl_display_roundtrip(state.display);
    if (SHOW_OUTPUTS)
        exit(EXIT_SUCCESS);
    if (wl_list_empty(&state.outputs)) {
        cflp_error(":/ sorry about this but we can't seem to find any output.");
        return EXIT_FAILURE;
    }

    // Main Loop
    while (true) {
        if (wl_display_flush(state.display) == -1 && errno != EAGAIN)
            break;

        struct pollfd fds[2];
        fds[0].fd = wl_display_get_fd(state.display);
        fds[0].events = POLLIN;
        fds[1].fd = wakeup_pipe[0];
        fds[1].events = POLLIN;

        // Wait for MPV to request a new frame to be drawn
        int poll_err = 0;
        while (poll_err == 0) {
            poll_err = poll(fds, sizeof(fds) / sizeof(fds[0]), 10);  // 10ms timeout
            if (poll_err == -1 && errno != EINTR)
                break;

            if (halt_info.stop_render_loop) {
                halt_info.stop_render_loop = 0;
                sleep(2); // Wait at least 2 secs to be killed
            }
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(state.display) == -1)
                break;
        }

        // MPV is ready to draw a new frame
        if (fds[1].revents & POLLIN) {
            // Empty the pipe
            char tmp[64];
            if (read(wakeup_pipe[0], tmp, sizeof(tmp)) == -1)
                break;

            mpv_render_context_update(mpv_glcontext);

            // Draw frame for all outputs
            struct display_output *output;
            wl_list_for_each(output, &state.outputs, link) {
                // Redraw immediately if not waiting for frame callback
                if (output->frame_callback == NULL) {
                    // Avoid crash when output is destroyed 
                    if (output->egl_window && output->egl_surface) {
                        if (VERBOSE == 2)
                            cflp_info("MPV is ready to render the next frame for %s", output->name);
                        render(output);
                    }
                } else {
                    output->redraw_needed = true;
                }
            }
        }
    }

    struct display_output *output, *tmp_output;
    wl_list_for_each_safe(output, tmp_output, &state.outputs, link) {
        destroy_display_output(output);
    }

    return EXIT_SUCCESS;
}
