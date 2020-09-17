#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <glad/glad.h>
#include <glad/glad_egl.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <cflogprinter.h>


struct wl_state {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct wl_list outputs;  // struct display_output::link
    char* monitor; // User selected output
    int surface_layer;
    int run_display;
};

struct display_output {
    uint32_t wl_name;
    struct wl_output *wl_output;
    struct zxdg_output_v1 *xdg_output;
    char *name;
    char *identifier;

    struct wl_state *state;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;

    uint32_t width, height;

    struct wl_list link;
};

static struct wl_egl_window* egl_window;
static EGLDisplay *egl_display;
static EGLContext *egl_context;
static EGLSurface *egl_surface;

static mpv_handle* mpv;
static mpv_render_context *mpv_glcontext;
static char* video_path;

static int VERBOSE = 0;

static void nop() {}

const static struct wl_callback_listener wl_surface_frame_listener;

static void render(struct display_output *output) {
    mpv_event* event = mpv_wait_event(mpv, 0);
    if (event->event_id == MPV_EVENT_SHUTDOWN || event->event_id == MPV_EVENT_IDLE) {
        exit(EXIT_SUCCESS);
    }

    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &(mpv_opengl_fbo){
            .fbo = 0,
            .w = output->width,
            .h = output->height,
        }},
        // Flip rendering (needed due to flipped GL coordinate system).
        {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
    };
    // Render frame
    mpv_render_context_render(mpv_glcontext, render_params);

    // Callback new frame
    struct wl_callback *callback = wl_surface_frame(output->surface);
    wl_callback_add_listener(callback, &wl_surface_frame_listener, output);

    // Display frame
    if (!eglSwapBuffers(egl_display, egl_surface)) {
        cflp_error("Failed to swap egl buffers 0x%X", eglGetError());
    }
}

static void frame_handle_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void) time;
    wl_callback_destroy(callback);
    render(data);
}

const static struct wl_callback_listener wl_surface_frame_listener = {
    .done = frame_handle_done,
};

static void *get_proc_address_mpv(void *ctx, const char *name){
    (void) ctx;
    return eglGetProcAddress(name);
}

static void init_mpv(struct display_output *output) {

    mpv = mpv_create();
    if (!mpv) {
        cflp_error("Failed creating mpv context");
        exit(EXIT_FAILURE);
    }

    // Load user configs
    const char *homedir = getenv("HOME");
    char *configcat = (char *) malloc(sizeof(homedir) + 30);

    char loaded_configs[50] = "";

    strcpy(configcat, homedir);
    if (mpv_load_config_file(mpv, strcat(configcat, "/.config/mpv/mpv.conf")) == 0)
        strcat(loaded_configs, "mpv.conf ");
    strcpy(configcat, homedir);
    if (mpv_load_config_file(mpv, strcat(configcat, "/.config/mpv/input.conf")) == 0)
        strcat(loaded_configs, "input.conf ");
    strcpy(configcat, homedir);
    if(mpv_load_config_file(mpv, strcat(configcat, "/.config/mpv/fonts.conf")) == 0)
        strcat(loaded_configs, "fonts.conf ");

    if (VERBOSE && strcmp(loaded_configs, ""))
        cflp_info("Loaded [ %s] user configs from \"~/.config\"", loaded_configs);

    // Enable user control through terminal
    mpv_set_option_string(mpv, "input-default-bindings", "yes");
    mpv_set_option_string(mpv, "input-terminal", "yes");
    mpv_set_option_string(mpv, "terminal", "yes");

    // Set mpv_options passed
    mpv_load_config_file(mpv, "/tmp/mpvpaper.conf");
    remove("/tmp/mpvpaper.conf");

    if (mpv_initialize(mpv) < 0) {
        cflp_error("mpv init failed");
        exit(EXIT_FAILURE);
    }
    // Have mpv render onto egl context
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_WL_DISPLAY, output->state->display},
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &(mpv_opengl_init_params){
            .get_proc_address = get_proc_address_mpv,
        }},
    };
    if (mpv_render_context_create(&mpv_glcontext, mpv, params) < 0)
        cflp_error("Failed to initialize mpv GL context");

    const char* cmd[] = {"loadfile", video_path, NULL};
    mpv_command_async(mpv, 0, cmd);

    mpv_event* event = mpv_wait_event(mpv, 1);
    while (event->event_id != MPV_EVENT_FILE_LOADED){
        event = mpv_wait_event(mpv, 1);
    }
    if (VERBOSE) {
        cflp_info("Loaded %s", video_path);
    }
}

static void init_egl(struct display_output *output) {

    egl_window = wl_egl_window_create(output->surface, output->width, output->height);
    if (!egl_display) {
        egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, output->state->display, NULL);
        eglInitialize(egl_display, NULL, NULL);
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

    EGLConfig config;
    EGLint config_len;
    eglChooseConfig(egl_display, win_attrib, &config, 1, &config_len);

    if (!egl_context) {
        // Check for OpenGL combatiblity for creating egl context
        static const struct { int major, minor; } gl_versions[] = {
            {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0},
            {3, 3}, {3, 2}, {3, 1}, {3, 0},
            {0, 0}
        };
        egl_context = NULL;
        for (int i = 0; gl_versions[i].major > 0; i++) {
            const EGLint ctx_attrib[] = {
                EGL_CONTEXT_MAJOR_VERSION, gl_versions[i].major,
                EGL_CONTEXT_MINOR_VERSION, gl_versions[i].major,
                EGL_NONE
            };
            egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attrib);
            if (egl_context) {
                if (VERBOSE) {
                    cflp_info("OpenGL %i.%i EGL context loaded", gl_versions[i].major, gl_versions[i].minor);
                }
                break;
            }
        }
        if (!egl_context) {
            cflp_error("Failed to create EGL context");
            exit(EXIT_FAILURE);
        }
    }

    egl_surface = eglCreatePlatformWindowSurface(egl_display, config, egl_window, NULL);
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    eglSwapInterval(egl_display, 0);

    gladLoadGLLoader((GLADloadproc) eglGetProcAddress);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glViewport(0, 0, output->width, output->height);
}

static void destroy_display_output(struct display_output *output) {
    if (!output) {
        return;
    }
    wl_list_remove(&output->link);
    if (output->layer_surface != NULL) {
        zwlr_layer_surface_v1_destroy(output->layer_surface);
    }
    if (output->surface != NULL) {
        wl_surface_destroy(output->surface);
    }
    if (egl_display && strcmp(output->name,output->state->monitor) == 0) {
        eglDestroySurface(egl_display, egl_surface);
        wl_egl_window_destroy(egl_window);
    }
    zxdg_output_v1_destroy(output->xdg_output);
    wl_output_destroy(output->wl_output);

    free(output->name);
    free(output->identifier);
    free(output);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
        uint32_t serial, uint32_t width, uint32_t height) {
    struct display_output *output = data;
    output->width = width;
    output->height = height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    // Setup render loop
    init_egl(output);
    if (!mpv) {
        init_mpv(output);
    }
    if (egl_display && mpv_glcontext) {
        // Start render loop
        render(output);
    }
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void) surface;

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
        output->state->layer_shell, output->surface, output->wl_output,
        output->state->surface_layer, "mpvpaper");

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

static void xdg_output_handle_name(void *data,
        struct zxdg_output_v1 *xdg_output, const char *name) {
    (void) xdg_output;

    struct display_output *output = data;
    output->name = strdup(name);
}

static void xdg_output_handle_description(void *data,
        struct zxdg_output_v1 *xdg_output, const char *description) {
    (void) xdg_output;

    struct display_output *output = data;

    // wlroots currently sets the description to `make model serial (name)`
    // If this changes in the future, this will need to be modified.
    char *paren = strrchr(description, '(');
    if (paren) {
        size_t length = paren - description;
        output->identifier = malloc(length);
        if (!output->identifier) {
            cflp_warning("Failed to allocate output identifier");
            return;
        }
        strncpy(output->identifier, description, length);
        output->identifier[length - 1] = '\0';
    }
}

static void xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output) {
    (void) xdg_output;

    struct display_output *output = data;

    if (strcmp(output->name, output->state->monitor) == 0 && !output->layer_surface) {
        if (VERBOSE)
            cflp_info("Output %s (%s) selected", output->name, output->identifier);
        create_layer_surface(output);
    }
    else {
        if (VERBOSE)
            cflp_warning("Output %s (%s) not selected", output->name, output->identifier);
        destroy_display_output(output);
    }
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = nop,
    .logical_size = nop,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
    .done = xdg_output_handle_done,
};

static void handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    (void) version;

    struct wl_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct display_output *output = calloc(1, sizeof(struct display_output));
        output->state = state;
        output->wl_name = name;
        output->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 3);

        wl_list_insert(&state->outputs, &output->link);

        if (state->run_display) {
            output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
                state->xdg_output_manager, output->wl_output);
            zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
        }
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name,
            &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        state->xdg_output_manager = wl_registry_bind(registry, name,
            &zxdg_output_manager_v1_interface, 2);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void) registry;

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

static void parse_command_line(int argc, char **argv, struct wl_state *state) {

    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"verbose", no_argument, NULL, 'v'},
        {"fork", no_argument, NULL, 'f'},
        {"layer", required_argument, NULL, 'l'},
        {"mpv-options", required_argument, NULL, 'o'},
        {0, 0, 0, 0}
    };

    const char *usage =
        "Usage: mpvpaper [options] <output> <url|path filename>\n"
        "Options:\n"
        "--help\t\t-h\tDisplays this help message\n"
        "--verbose\t-v\tBe more verbose\n"
        "--fork\t\t-f\tForks mpvpaper so you can close the terminal\n"
        "--layer\t\t-l\tSpecifies shell surface layer to run on (background by default)\n"
        "--mpv-options\t-o\tForwards mpv options (Must be within quotes\"\")\n";


    if(argc > 2) {
        char* layer_name;
        char* mpv_options;

        char opt;
        while((opt = getopt_long(argc, argv, "hvfl:o:", long_options, NULL)) != -1) {

            switch (opt) {
            case 'h':
                fprintf(stdout, "%s", usage);
                exit(EXIT_SUCCESS);
            case 'v':
                VERBOSE = 1;
                break;
            case 'f':
                if(fork() > 0) {
                    exit(EXIT_SUCCESS);
                }
                fclose(stdout);
                fclose(stderr);
                fclose(stdin);
                break;
            case 'l':
                layer_name = strdup(optarg);
                if(layer_name == NULL) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
                } else if(strcasecmp(layer_name, "top") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
                } else if(strcasecmp(layer_name, "bottom") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
                } else if(strcasecmp(layer_name, "background") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
                } else if(strcasecmp(layer_name, "overlay") == 0) {
                    state->surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
                } else {
                    cflp_error("%s is not a shell surface layer", layer_name);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'o':
                mpv_options = optarg;
                // Forward to a tmp file so mpv can parse options
                for (int i = 0; i < (int)strlen(mpv_options); i++) {
                    if (mpv_options[i] == ' ') mpv_options[i] = '\n';
                }
                FILE* file = fopen("/tmp/mpvpaper.conf", "w");
                fputs(mpv_options, file);
                fclose(file);
                break;
            }
        }
        if(optind + 1 >= argc) {
            fprintf(stderr, "%s", usage);
            exit(EXIT_FAILURE);
        }
        state->monitor = strdup(argv[optind]);

        video_path = strdup(argv[optind+1]);

        if(!system("pidof swaybg > /dev/null")) {
            cflp_warning("swaybg is running. This may block mpvpaper from being seen.");
        }
    }
    else {
        fprintf(stderr, "%s", usage);
        exit(EXIT_FAILURE);
    }

}
int main(int argc, char **argv) {

    struct wl_state state = {0};
    wl_list_init(&state.outputs);

    parse_command_line(argc, argv, &state);

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        cflp_error("Unable to connect to the compositor. "
                "If your compositor is running, check or set the "
                "WAYLAND_DISPLAY environment variable.");
        return EXIT_FAILURE;
    }

    struct wl_registry *registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);
    if (state.compositor == NULL || state.layer_shell == NULL ||
            state.xdg_output_manager == NULL) {
        cflp_error("Missing a required Wayland interface");
        return EXIT_FAILURE;
    }

    struct display_output *output;
    wl_list_for_each(output, &state.outputs, link) {
        output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
            state.xdg_output_manager, output->wl_output);
        zxdg_output_v1_add_listener(output->xdg_output,
            &xdg_output_listener, output);
    }

    // Check outputs
    wl_display_roundtrip(state.display);
    if (wl_list_empty(&state.outputs)) {
        cflp_error(":/ sorry about this but we can't seem to find any output.");
        return EXIT_FAILURE;
    }

    state.run_display = 1;
    while (wl_display_dispatch(state.display) != -1 && state.run_display) {
        // NOP
    }

    struct display_output *tmp_output;
    wl_list_for_each_safe(output, tmp_output, &state.outputs, link) {
        destroy_display_output(output);
    }

    return EXIT_SUCCESS;
}
