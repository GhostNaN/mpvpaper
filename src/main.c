#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include <wayland-client.h>
#include <wayland-egl.h>

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
    struct wl_list outputs; // struct display_output::link
    struct wl_list toplevel_handles;
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

struct toplevel_handle_state {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *title;
    bool is_blocking;

    struct wl_list link;
};

static EGLConfig egl_config;
static EGLDisplay *egl_display;
static EGLContext *egl_context;

static mpv_handle *mpv;
static mpv_render_context *mpv_glcontext;
static int wakeup_fd;
static char *video_path;
static char *mpv_options = "";

static struct {
    char **pauselist;
    char **stoplist;

    int argc;
    char **argv_copy;
    char *save_info;

    int auto_pause;
    int auto_stop;

    bool list_paused;
    bool auto_paused;
    bool full_paused;
    bool user_paused;
    bool mpv_paused;

    bool frame_ready;
    bool stop_render_loop;

} halt_info = {NULL, NULL, 0, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static pthread_t threads[5] = {0};
static pthread_mutex_t halt_mutex = PTHREAD_MUTEX_INITIALIZER;


static uint SLIDESHOW_TIME = 0;
static bool SHOW_OUTPUTS = false;
static int VERBOSE = 0;

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

    if (wakeup_fd >= 0)
        close(wakeup_fd);
}

static void exit_mpvpaper(int reason) {
    if (VERBOSE)
        cflp_info("Exiting mpvpaper");
    exit_cleanup();
    exit(reason);
}

static void *exit_by_pthread(void *_) {
    exit_mpvpaper(EXIT_SUCCESS);
    pthread_exit(NULL);
}

static void handle_signal(int signum) {
    (void)signum;
    // Separate thread to avoid crash
    pthread_t thread;
    pthread_create(&thread, NULL, exit_by_pthread, NULL);
}

const static struct wl_callback_listener wl_surface_frame_listener;

static void render(struct display_output *output) {
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &(mpv_opengl_fbo) {
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
        cflp_error("Failed to make output surface current %s", eglGetErrorString(eglGetError()));

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
        cflp_error("Failed to swap egl buffers %s", eglGetErrorString(eglGetError()));
    else {
        // Inform libmpv that the buffer has been presented so it can release any
        // associated GL fence objects and resources
        if (mpv_glcontext)
            mpv_render_context_report_swap(mpv_glcontext);
    }
}

static void frame_handle_done(void *data, struct wl_callback *callback, uint32_t frame_time) {
    (void)frame_time;
    struct display_output *output = data;
    wl_callback_destroy(callback);

    // Display is ready for new frame
    output->frame_callback = NULL;

    // Reset deadman switch timer
    pthread_mutex_lock(&halt_mutex);
    halt_info.frame_ready = 1;
    pthread_mutex_unlock(&halt_mutex);

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

    char **new_argv = calloc(halt_info.argc + 3, sizeof(char *)); // Plus 3 for adding in -Z
    if (!new_argv) {
        cflp_error("Failed to allocate new argv");
        exit(EXIT_FAILURE);
    }

    uint i = 0;
    for (i=0; i < halt_info.argc; i++) {
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

// Update pause flags and mpv state safely
static void update_mpv_pause_state(bool *pause_flag, bool new_flag_state, const char *reason) {
    pthread_mutex_lock(&halt_mutex);

    *pause_flag = new_flag_state;

    bool call_pause = (
        halt_info.list_paused ||
        halt_info.auto_paused ||
        halt_info.user_paused ||
        halt_info.full_paused);

    // Only command MPV if the aggregate state changes
    if (call_pause != halt_info.mpv_paused) {
        halt_info.mpv_paused = call_pause;

        if (new_flag_state && reason && VERBOSE)
            cflp_info("Pause triggered by: %s", reason);
        else if (reason && VERBOSE)
            cflp_info("Pause cleared by: %s", reason);

        mpv_command_async(mpv, 0, (const char *[]){
            "set",
            "pause",
            call_pause ? "yes" : "no",
            NULL
        });
    }

    pthread_mutex_unlock(&halt_mutex);
}

static char *check_watch_list(char **list) {

    char pid_name[512] = {0};

    for (uint i=0; list[i] != NULL; i++) {
        snprintf(pid_name, sizeof(pid_name), "pidof %s > /dev/null", list[i]);

        // Stop if program is open
        if (!system(pid_name))
            return list[i];
    }
    return NULL;
}

static void *monitor_pauselist(void *_) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    bool list_paused = 0;

    while (halt_info.pauselist) {

        char *app = check_watch_list(halt_info.pauselist);
        if (app && !list_paused && !halt_info.list_paused && !halt_info.mpv_paused) {
            update_mpv_pause_state(&halt_info.list_paused, true, app);
            list_paused = 1;
        } else if (!app && list_paused) {
            update_mpv_pause_state(&halt_info.list_paused, false, "Blocking Apps");
            list_paused = 0;
        }


        pthread_sleep(1);
    }
    pthread_exit(NULL);
}

static void *monitor_stoplist(void *_) {
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

static void *handle_auto_pause(void *_) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    while (halt_info.auto_pause) {

        // Set deadman switch timer
        pthread_mutex_lock(&halt_mutex);
        halt_info.frame_ready = 0;
        pthread_mutex_unlock(&halt_mutex);

        pthread_sleep(2);

        if (!halt_info.frame_ready && !halt_info.auto_paused && !halt_info.mpv_paused) {
            update_mpv_pause_state(&halt_info.auto_paused, true, "Frame Callback");

            while (!halt_info.frame_ready) {
                pthread_usleep(10000);
            }
            if (halt_info.auto_paused)
                update_mpv_pause_state(&halt_info.auto_paused, false, "Frame Callback");
        }
    }
    pthread_exit(NULL);
}

static void *handle_auto_stop(void *_) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    while (halt_info.auto_stop) {

        // Set deadman switch timer
        pthread_mutex_lock(&halt_mutex);
        halt_info.frame_ready = 0;
        pthread_mutex_unlock(&halt_mutex);

        pthread_sleep(2);

        // While annoying, pausing also causes the frame to not callback and must be ignored
        if (!halt_info.frame_ready && !halt_info.mpv_paused) {
            if (VERBOSE)
                cflp_info("Stopping because mpvpaper is hidden");
            stop_mpvpaper();
        }
    }
    pthread_exit(NULL);
}

static void *handle_mpv_events(void *_) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    int mpv_paused = 0;
    time_t start_time = time(NULL);

    const int MPV_OBSERVE_PAUSE = 1;
    mpv_observe_property(mpv, MPV_OBSERVE_PAUSE, "pause", MPV_FORMAT_FLAG);

    while (!halt_info.stop_render_loop) {
        if (SLIDESHOW_TIME) {
            if ((time(NULL) - start_time) >= SLIDESHOW_TIME) {
                mpv_command_async(mpv, 0, (const char *[]){"playlist-next", NULL});
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
                    if (!halt_info.list_paused && !halt_info.auto_paused && !halt_info.full_paused)
                        update_mpv_pause_state(&halt_info.user_paused, true, NULL);
                } else { // Clear paused checks if not paused
                    update_mpv_pause_state(&halt_info.user_paused, false, NULL);
                }
            }
        }

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
    // Enable user control through terminal by default and configs
    mpv_set_option_string(mpv, "input-default-bindings", "yes");
    mpv_set_option_string(mpv, "input-terminal", "yes");
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "config", "yes");
    mpv_set_option_string(mpv, "background-color", "#00000000");

    // Convenience options passed for slideshow mode
    if (SLIDESHOW_TIME != 0) {
        mpv_set_option_string(mpv, "loop", "yes");
        mpv_set_option_string(mpv, "loop-playlist", "yes");
    }

    // Set mpv_options passed
    if (strcmp(mpv_options, "") != 0) {
        // Create config file name
        char *opt_config_path = NULL;
        // Use getpid() to avoid possible race condition with another mpvpaper instance running
        if (asprintf(&opt_config_path, "/tmp/mpvpaper_%d.config", getpid()) < 0) {
            cflp_error("Failed to create file path for mpv options config");
            exit_mpvpaper(EXIT_FAILURE);
        }

        // Put options into config file
        FILE *file = fopen(opt_config_path, "w");
        fputs(mpv_options, file);
        fclose(file);

        mpv_load_config_file(mpv, opt_config_path);
        remove(opt_config_path);
        free(opt_config_path);
    }
}

// Sync fence tracking wrapper to resolve libmpv memory/descriptor leaks by returning NULL
static GLsync APIENTRY wrap_glFenceSync(GLenum condition, GLbitfield flags) {
    (void)condition;
    (void)flags;
    return NULL;
}

static void *get_proc_address_mpv(void *ctx, const char *name) {
    (void)ctx;
    if (strcmp(name, "glFenceSync") == 0) {
        return (void *)wrap_glFenceSync; // Redirect to wrapper returning NULL
    }
    return eglGetProcAddress(name);
}

static void render_update_callback(void *callback_ctx) {
    (void)callback_ctx;
    uint64_t inc = 1;
    if (write(wakeup_fd, &inc, sizeof(inc)) < 0) {
        cflp_error("Failed to write to wakeup eventfd");
        exit_mpvpaper(EXIT_FAILURE);
    }
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

    // Run again after mpv_initialize to override options in config files
    set_init_mpv_options(state);

    // Force libmpv vo as nothing else will work
    char *vo_option = mpv_get_property_string(mpv, "options/vo");
    if (strcmp(vo_option, "libmpv") != 0) {
        if (strcmp(vo_option, "") != 0) {
            cflp_warning("mpvpaper does not support any other vo than \"libmpv\"");
        }
        mpv_set_option_string(mpv, "vo", "libmpv");
    }
    mpv_free(vo_option);

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
        mpv_command(mpv, (const char *[]){"set", "start", time_pos, NULL});
        // Recover playlist pos, that is if it's not shuffled...
        mpv_command(mpv, (const char *[]){"set", "playlist-start", playlist_pos, NULL});
    }

    // Load media or try loading as playlist file
    if (strstr(video_path, "--playlist=") == NULL) {
        mpv_err = mpv_command(mpv, (const char *[]){"loadfile", video_path, NULL});
    } else {
        // cut out "--playlist=" then load as a list file
        mpv_err = mpv_command(mpv, (const char *[]){"loadlist", video_path + strlen("--playlist="), NULL});
    }

    if (mpv_err < 0) {
        cflp_error("Failed to load file, %s", mpv_error_string(mpv_err));
        exit_mpvpaper(EXIT_FAILURE);
    }

    // Wait for file to load
    mpv_event *event = mpv_wait_event(mpv, 1);
    while (event->event_id != MPV_EVENT_FILE_LOADED) {
        event = mpv_wait_event(mpv, 1);
    }
    if (VERBOSE)
        cflp_info("Loaded %s", video_path);

    // Return start pos to default
    if (default_start) {
        mpv_command(mpv, (const char *[]){"set", "start", default_start, NULL});
        mpv_free(default_start);
    }

    // mpv must never idle
    mpv_command(mpv, (const char *[]){"set", "idle", "no", NULL});

    mpv_render_context_set_update_callback(mpv_glcontext, render_update_callback, NULL);
}

static void init_egl(struct wl_state *state) {
    egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, state->display, NULL);
    if (egl_display == EGL_NO_DISPLAY) {
        cflp_error("Failed to get EGL display");
        exit_mpvpaper(EXIT_FAILURE);
    }
    if (!eglInitialize(egl_display, NULL, NULL)) {
        cflp_error("Failed to initialize EGL %s", eglGetErrorString(eglGetError()));
        exit_mpvpaper(EXIT_FAILURE);
    }

    eglBindAPI(EGL_OPENGL_API);
    const EGLint win_attrib[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLint num_config;
    if (!eglChooseConfig(egl_display, win_attrib, &egl_config, 1, &num_config)) {
        cflp_error("Failed to set EGL frame buffer config %s", eglGetErrorString(eglGetError()));
        exit_mpvpaper(EXIT_FAILURE);
    }

    // Check for OpenGL compatibility for creating egl context
    static const struct { int major, minor; } gl_versions[] = {
        {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0},
        {3, 3}, {3, 2}, {3, 1}, {3, 0},
        {0, 0}
    };
    egl_context = NULL;
    for (uint i=0; gl_versions[i].major > 0; i++) {
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
        cflp_error("Failed to create EGL context %s", eglGetErrorString(eglGetError()));
        exit_mpvpaper(EXIT_FAILURE);
    }

    if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context)) {
        cflp_error("Failed to make context current %s", eglGetErrorString(eglGetError()));
        exit_mpvpaper(EXIT_FAILURE);
    }

    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
        cflp_error("Failed to load OpenGL %s", eglGetErrorString(eglGetError()));
        exit_mpvpaper(EXIT_FAILURE);
    }
}

static struct toplevel_handle_state *match_toplevel_handle(struct wl_state *wl_state,
        struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle) {

    struct toplevel_handle_state *handle_state;
    wl_list_for_each(handle_state, &wl_state->toplevel_handles, link) {

        if(handle_state->handle == toplevel_handle)
            return handle_state;
    }

    return NULL;
}

static void toplevel_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
        const char *title) {

    struct wl_state *wl_state = data;

    struct toplevel_handle_state *handle_state = match_toplevel_handle(wl_state, toplevel_handle);
    if (!handle_state) return;

    if (handle_state->title) free(handle_state->title);

    handle_state->title = strdup(title);
}

static void toplevel_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
        const char *app_id) { /* NOP */ }

static void toplevel_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
        struct wl_output *output) { /* NOP */ }

static void toplevel_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
        struct wl_output *output) { /* NOP */ }

static void check_handle_blocking(struct toplevel_handle_state *handle_state, struct wl_state *wl_state,
        bool currently_blocking) {

    if (handle_state->is_blocking != currently_blocking) {
        handle_state->is_blocking = currently_blocking;

        // Calculate if ANY window is still blocking the screen
        bool any_blocking = false;
        struct toplevel_handle_state *iter_handle;
        wl_list_for_each(iter_handle, &wl_state->toplevel_handles, link) {
            if (iter_handle->is_blocking) {
                any_blocking = true;
                break;
            }
        }

        if (any_blocking) {
            if (halt_info.auto_stop) {
                if (VERBOSE)
                    cflp_info("Stopping for %s", iter_handle->title);

                stop_mpvpaper();
            }
            if (!halt_info.full_paused && !halt_info.mpv_paused)
                update_mpv_pause_state(&halt_info.full_paused, true, iter_handle->title);

        } else { // No windows are blocking anymore
            update_mpv_pause_state(&halt_info.full_paused, false, "Blocking Windows");
        }
    }
}

static void toplevel_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
        struct wl_array *state) {

    struct wl_state *wl_state = data;
    struct toplevel_handle_state *handle_state = match_toplevel_handle(wl_state, toplevel_handle);
    if (!handle_state) return;

    uint32_t *s;
    bool currently_blocking = false;

    wl_array_for_each(s, state) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) {
            currently_blocking = true;
            break;
        } else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) {
            if (halt_info.auto_pause > 2 || halt_info.auto_stop > 2) {
                currently_blocking = true;
                break;
            }
        }
    }

    check_handle_blocking(handle_state, wl_state, currently_blocking);
}

static void toplevel_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle) { /* NOP */ }

static void toplevel_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle) {

    struct wl_state *wl_state = data;

    struct toplevel_handle_state *handle_state = match_toplevel_handle(wl_state, toplevel_handle);
    if (!handle_state) return;

    if(handle_state->is_blocking)
        check_handle_blocking(handle_state, wl_state, false);

    // Destroy handle
    if (handle_state->title) free(handle_state->title);
    wl_list_remove(&handle_state->link);
}

static void toplevel_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
        struct zwlr_foreign_toplevel_handle_v1 *parent) { /* NOP */ }

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
    .title = toplevel_title,
    .app_id = toplevel_app_id,
    .output_enter = toplevel_output_enter,
    .output_leave = toplevel_output_leave,
    .state = toplevel_state,
    .done = toplevel_done,
    .closed = toplevel_closed,
    .parent = toplevel_parent,
};

static void toplevel_created(void *data, struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager,
        struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle) {

    struct wl_state *state = data;

    struct toplevel_handle_state *handle_state = calloc(1, sizeof(struct toplevel_handle_state));

    handle_state->handle = toplevel_handle;
    wl_list_insert(&state->toplevel_handles, &handle_state->link);

    zwlr_foreign_toplevel_handle_v1_add_listener(handle_state->handle, &toplevel_handle_listener, state);
}

static void toplevel_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager) { /* NOP */ }

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
    .toplevel = toplevel_created,
    .finished = toplevel_finished,
};

static void destroy_display_output(struct display_output *output) {
    if (!output) return;

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

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t width,
        uint32_t height) {

    struct display_output *output = data;
    output->width = width;
    output->height = height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    wl_surface_set_buffer_scale(output->surface, output->scale);

    // Ignore bad surfaces
    if (width == 0 || height == 0) return;

    if (!output->egl_window) {
        output->egl_window = wl_egl_window_create(output->surface, output->width * output->scale,
                output->height * output->scale);
        output->egl_surface = eglCreatePlatformWindowSurface(egl_display, egl_config, output->egl_window, NULL);
        if (!output->egl_surface) {
            cflp_error("Failed to create EGL surface for %s %s", output->name, eglGetErrorString(eglGetError()));
            destroy_display_output(output);
            return;
        }

        if (!eglMakeCurrent(egl_display, output->egl_surface, output->egl_surface, egl_context))
            cflp_error("Failed to make output surface current %s", eglGetErrorString(eglGetError()));
        eglSwapInterval(egl_display, 0);

        // After making EGL_NO_SURFACE current to a context
        // Only with the Nvidia Pro drivers will set the draw buffer state to GL_NONE
        // So we are going to force GL_BACK just like Mesa's EGL implementation
        glDrawBuffer(GL_BACK);

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

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
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
    );
    zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(output->layer_surface, &layer_surface_listener, output);
    wl_surface_commit(output->surface);
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width,
        int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) { /* NOP */ }

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height,
        int32_t refresh) { /* NOP */ }

static void output_done(void *data, struct wl_output *wl_output) {
    (void)wl_output;

    struct display_output *output = data;

    // Find what monitors are listed seprated by spaces
    char *monitor_copy = strdup(output->state->monitor);
    bool name_ok = false;

    for (char *tok = strtok(monitor_copy, " \t"); tok; tok = strtok(NULL, " \t")) {
        if (strcmp(tok, output->name) == 0) {
            name_ok = true;
            break;
        }
    }
    free(monitor_copy);

    if (output->identifier) // Some compositors don't have an identifier for some reason
        name_ok = name_ok || (output->identifier[0] && strstr(output->state->monitor, output->identifier) != NULL);

    // Check for all other outputs types
    name_ok = name_ok ||
        strcmp(output->state->monitor, "*") == 0 ||
        strcasecmp(output->state->monitor, "all") == 0;


    if (name_ok && !output->layer_surface) {
        if (VERBOSE)
            cflp_info("Output %s (%s) selected", output->name, output->identifier);
        create_layer_surface(output);
    }
    if (!name_ok || (strcmp(output->state->monitor, "") == 0)) {
        if (SHOW_OUTPUTS) {
            if (output->name && output->identifier)
                cflp_info("Output: %s  Identifier: %s", output->name, output->identifier);
            else if (output->name)
                cflp_info("Output: %s", output->name);
        }
        destroy_display_output(output);
    }
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t scale) {
    (void)wl_output;

    struct display_output *output = data;
    output->scale = scale;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)wl_output;

    struct display_output *output = data;
    output->name = strdup(name);
}

static void output_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)wl_output;

    struct display_output *output = data;

    // wlroots currently sets the description to `make model serial (name)`
    // Having `(name)` is redundant and must be removed to have a clean identifier.
    // If this changes in the future, this will need to be modified.
    const char *paren = strrchr(description, '(');
    if (paren) {
        size_t length = paren - description;
        output->identifier = calloc(length, sizeof(char));
        if (!output->identifier) {
            cflp_warning("Failed to allocate output identifier");
            return;
        }
        strncpy(output->identifier, description, length);
        output->identifier[length - 1] = '\0';
    } else {
        output->identifier = strdup(description);
    }
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
        uint32_t version) {
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
    if (halt_info.auto_pause > 1 || halt_info.auto_stop > 1) {
        if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
            struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
            toplevel_manager = wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 3);
            zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager, &toplevel_manager_listener, state);
        }
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
        // Dynamically realloc for each program added to list
        char app[512];
        char **list = NULL;
        uint i = 0;
        for (i=0; fscanf(file, "%511s", app) != EOF; i++) {
            list = realloc(list, (i+1) * sizeof(char *));
            if (!list) {
                cflp_error("Failed to reallocate watch list");
                exit(EXIT_FAILURE);
            }
            list[i] = strdup(app);
        }
        // Null terminate
        list = realloc(list, (i+1) * sizeof(char *));
        list[i] = NULL;

        fclose(file);
        // If any app found
        if (list[0])
            return list;
    }
    return NULL;
}

static void copy_argv(int argc, char *argv[]) {
    halt_info.argc = argc;
    halt_info.argv_copy = calloc(argc+1, sizeof(char *));
    if (!halt_info.argv_copy) {
        cflp_error("Failed to allocate argv copy");
        exit(EXIT_FAILURE);
    }

    int j = 0;
    for (uint i=0; i < argc; i++) {
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

    char *pause_path = NULL;
    if (asprintf(&pause_path, "%s/.config/mpvpaper/pauselist", home_dir) < 0) {
        cflp_error("Failed to create file path for pauselist");
        exit(EXIT_FAILURE);
    }
    halt_info.pauselist = get_watch_list(pause_path);
    free(pause_path);

    char *stop_path = NULL;
    if (asprintf(&stop_path, "%s/.config/mpvpaper/stoplist", home_dir) < 0) {
        cflp_error("Failed to create file path for stoplist");
        exit(EXIT_FAILURE);
    }
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
        {"auto-mode", required_argument, NULL, 'a'},
        {"slideshow", required_argument, NULL, 'n'},
        {"layer", required_argument, NULL, 'l'},
        {"mpv-options", required_argument, NULL, 'o'},
        {0, 0, 0, 0}
    };

    const char *usage =
        "Usage: mpvpaper [options] <output> <url|path filename>\n"
        "\n"
        "Example: mpvpaper -vs -a full -o \"no-audio loop\" DP-2 /path/to/video\n"
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
        "--auto-mode    -a FULL/MAX     Extends above auto options to also check if any window is found to be:\n"
        "                               [FULL = fullscreen] or [MAX = fullscreen or maximized]\n"
        "--slideshow    -n SECS         Slideshow mode plays the next video in a playlist every ? seconds\n"
        "                               And passes mpv options \"loop loop-playlist\" for convenience\n"
        "--layer        -l LAYER        Specifies shell surface layer to run on (background by default)\n"
        "--mpv-options  -o \"OPTIONS\"    Forwards mpv options (Must be within quotes\"\")\n"
        "\n"
        "* The auto options might not work as intended\n"
        "See the man page for more details\n";

    char *layer_name;
    int auto_mode = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "hdvfpsa:n:l:o:Z:", long_options, NULL)) != -1) {

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

                if (halt_info.auto_stop) {
                    cflp_warning("You cannot use auto-stop and auto-pause together");
                    halt_info.auto_stop = 0;
                }
                break;
            case 's':
                halt_info.auto_stop = 1;

                if (halt_info.auto_pause) {
                    cflp_warning("You cannot use auto-pause and auto-stop together");
                    halt_info.auto_pause = 0;
                }
                break;
            case 'a':
                if (strcasecmp(optarg, "full") == 0) auto_mode = 2;
                else if (strcasecmp(optarg, "max") == 0) auto_mode = 3;
                else {
                    cflp_error("Neither FULL or MAX was selected for the auto-mode\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'n':
                SLIDESHOW_TIME = atoi(optarg);
                if (SLIDESHOW_TIME == 0)
                    cflp_warning("0 or invalid time set for slideshow\n"
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
                    cflp_error("%s is not a shell surface layer\n"
                                      "Your options are: top, bottom, background and overlay", layer_name);
                    exit(EXIT_FAILURE);
                }

                free(layer_name);
                break;
            case 'o':
                mpv_options = strdup(optarg);
                // Split options by newline handling quotes and escaped characters
                bool in_single_quotes = 0, in_double_quotes = 0, escape_next_char = 0;
                int write_index = 0;
                for (uint i = 0; mpv_options[i] != '\0'; i++) {
                    if (escape_next_char) {
                        mpv_options[write_index++] = mpv_options[i];
                        escape_next_char = 0;
                        continue;
                    }

                    switch (mpv_options[i]) {
                        case '\\':
                            if (!in_single_quotes) {
                                escape_next_char = 1;
                            } else {
                                mpv_options[write_index++] = mpv_options[i];
                            }
                            break;
                        case '"':
                            if (!in_single_quotes)
                                in_double_quotes = !in_double_quotes;
                            mpv_options[write_index++] = mpv_options[i];
                            break;
                        case '\'':
                            if (!in_double_quotes)
                                in_single_quotes = !in_single_quotes;
                            mpv_options[write_index++] = mpv_options[i];
                            break;
                        case ' ':
                            if (!in_single_quotes && !in_double_quotes) {
                                mpv_options[write_index++] = '\n'; // Replace space with newline
                            } else {
                                mpv_options[write_index++] = mpv_options[i];
                            }
                            break;
                        default:
                            mpv_options[write_index++] = mpv_options[i];
                            break;
                    }
                }
                mpv_options[write_index] = '\0';
                break;
            case 'Z': // Hidden option to recover video pos after stopping
                halt_info.save_info = strdup(optarg);
                break;
        }
    }

    if (VERBOSE)
        cflp_info("Verbose Level %i enabled", VERBOSE);

    // Put in auto_mode after loop to allow out of order options
    if (auto_mode != 0) {
        if (halt_info.auto_pause) {
            halt_info.auto_pause = auto_mode;
        } else if (halt_info.auto_stop) {
            halt_info.auto_stop = auto_mode;
        } else {
            cflp_error("The auto-mode option requires either auto-pause or auto-stop to be enabled\n");
            exit(EXIT_FAILURE);
        }
    }

    // Need at least a output and file or playlist file
    char *playlist_opt_pointer;
    if ((playlist_opt_pointer = strstr(mpv_options, "--playlist=")) != NULL) {

        // cut out mpv "--playlist=/my/list.txt" option from mpv_options as video_path, cut out "--playlist="  later
        video_path = strtok(strdup(playlist_opt_pointer), "\n");

        // remove mpv "--playlist=" option to avoid "The playlist option can't be used in a config file."
        char *playlist_opt_pointer_tail = playlist_opt_pointer + strlen(video_path);
        memmove(playlist_opt_pointer, playlist_opt_pointer_tail, strlen(playlist_opt_pointer_tail)+1);

        if (optind >= argc) {
            cflp_error("Not enough args passed\n"
                              "Please set output");
            fprintf(stderr, "%s", usage);
            exit(EXIT_FAILURE);
        }
    } else if (optind+1 >= argc) {
        cflp_error("Not enough args passed\n"
                          "Please set output and url|path filename");
        fprintf(stderr, "%s", usage);
        exit(EXIT_FAILURE);
    }

    state->monitor = strdup(argv[optind]);
    if (!video_path)
        video_path = strdup(argv[optind+1]);
}

static void check_paper_processes() {
    // Check for other wallpaper process running
    const char *other_wallpapers[] = {"swaybg", "glpaper", "hyprpaper", "wpaperd", "swww-daemon"};
    char wallpaper_sbuffer[64] = {0};

    for (uint i=0; i < sizeof(other_wallpapers) / sizeof(other_wallpapers[0]); i++) {
        snprintf(wallpaper_sbuffer, sizeof(wallpaper_sbuffer), "pidof %s > /dev/null", other_wallpapers[i]);

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
    wl_list_init(&state.toplevel_handles);

    parse_command_line(argc, argv, &state);
    set_watch_lists();
    if (halt_info.auto_stop || halt_info.stoplist)
        copy_argv(argc, argv);

    // Create eventfd for checking render_update_callback()
    wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    if (wakeup_fd == -1) {
        cflp_error("Creating eventfd failed.");
        return EXIT_FAILURE;
    }

    // Connect to Wayland compositor
    state.display = wl_display_connect(NULL);
    if (!state.display) {
        cflp_error("Unable to connect to the compositor.\n"
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
        struct pollfd fds[2];
        fds[0].fd = wl_display_get_fd(state.display);
        fds[0].events = POLLIN;
        fds[1].fd = wakeup_fd;
        fds[1].events = POLLIN;

        // First make sure to call wl_display_prepare_read() before poll() to avoid deadlock
        int wl_display_prepare_read_state = wl_display_prepare_read(state.display);

        // Next flush just before poll()
        if (wl_display_flush(state.display) == -1 && errno != EAGAIN)
            break;

        // Wait for a mpv callback or wl_display event within 10ms
        if (poll(fds, sizeof(fds) / sizeof(fds[0]), 10) == -1 && errno != EINTR)
            break;

        // If wl_display_prepare_read() was successful as 0
        if (wl_display_prepare_read_state == 0) {
            // Read if we have wl_display events after poll()
            if (fds[0].revents & POLLIN) {
                wl_display_read_events(state.display);
            } else { // Otherwise we must cancel the read
                wl_display_cancel_read(state.display);
            }
        }
        // Lastly process wl_display events without blocking
        if (wl_display_dispatch_pending(state.display) == -1)
            break;

        if (halt_info.stop_render_loop) {
            halt_info.stop_render_loop = 0;
            sleep(2); // Wait at least 2 secs to be killed
        }

        // MPV is ready to draw a new frame
        if (fds[1].revents & POLLIN) {
            // Empty the eventfd
            uint64_t tmp;
            if (read(wakeup_fd, &tmp, sizeof(tmp)) == -1)
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
    wl_list_for_each_safe(output, tmp_output, &state.outputs, link) { destroy_display_output(output); }

    return EXIT_SUCCESS;
}
