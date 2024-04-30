#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <wayland-client.h>

typedef unsigned int uint;

struct wl_state {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_list outputs; // struct display_output::link
    char *monitor; // User selected output
};

struct display_output {
    uint32_t wl_name;
    struct wl_output *wl_output;
    char *name;

    struct wl_state *state;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;

    uint32_t width, height;

    struct wl_list link;
};

static struct {
    char **argv_copy;
    char **stoplist;
    bool auto_stop;

    int start_time;
} halt_info = {NULL, NULL, false, 0};

static void nop() {}

static void revive_mpvpaper() {
    // Get the "real" cwd
    char exe_dir[1024];
    int cut_point = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir));
    for (uint i=cut_point; i > 1; i--) {
        if (exe_dir[i] == '/') {
            exe_dir[i+1] = '\0';
            break;
        }
    }

    execv(strcat(exe_dir, "mpvpaper"), halt_info.argv_copy);
}

static void check_stoplist() {
    for (uint i=0; halt_info.stoplist[i] != NULL; i++) {
        char pid_name[512] = {0};
        strcpy(pid_name, "pidof ");
        strcat(pid_name, halt_info.stoplist[i]);
        strcat(pid_name, " > /dev/null");

        while (!system(pid_name))
            usleep(100000); // 0.1 sec
    }
    if (!halt_info.auto_stop)
        revive_mpvpaper();
}

static struct wl_buffer *create_dummy_buffer(struct display_output *output) {
    const int WIDTH = 1, HEIGHT = 1;

    int stride = WIDTH * 4; // 4 bytes per pixel
    int size = stride * HEIGHT;

    // Create shm
    const char SHM_NAME[] = "/wl_shm-dummy";
    shm_unlink(SHM_NAME);
    int fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    shm_unlink(SHM_NAME);
    if (ftruncate(fd, size) < 0) {
        fprintf(stderr, "Failed to truncate shm");
        exit(EXIT_FAILURE);
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(output->state->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);

    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

const static struct wl_callback_listener wl_surface_frame_listener;

static void create_surface_frame(struct display_output *output) {

    struct wl_buffer *dummy_buffer = create_dummy_buffer(output);

    // Callback new frame
    struct wl_callback *callback = wl_surface_frame(output->surface);
    wl_callback_add_listener(callback, &wl_surface_frame_listener, output);
    wl_surface_attach(output->surface, dummy_buffer, 0, 0);
    wl_surface_damage(output->surface, 0, 0, output->width, output->height);
    wl_surface_commit(output->surface);

    wl_buffer_destroy(dummy_buffer);
}

static void frame_handle_done(void *data, struct wl_callback *callback, uint32_t frame_time) {
    (void)frame_time;
    wl_callback_destroy(callback);

    if (halt_info.stoplist) {
        check_stoplist();
        // If checking stoplist and frame callback took longer than a second don't revive
        if (frame_time - halt_info.start_time < 1000)
            revive_mpvpaper();
    } else {
        revive_mpvpaper();
    }

    halt_info.start_time = frame_time;
    create_surface_frame(data);
}

const static struct wl_callback_listener wl_surface_frame_listener = {
    .done = frame_handle_done,
};

static void destroy_display_output(struct display_output *output) {
    if (!output)
        return;
    wl_list_remove(&output->link);
    if (output->layer_surface != NULL)
        zwlr_layer_surface_v1_destroy(output->layer_surface);
    if (output->surface != NULL)
        wl_surface_destroy(output->surface);
    wl_output_destroy(output->wl_output);

    free(output->name);
    free(output);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, 
        uint32_t width, uint32_t height) {

    struct display_output *output = data;
    output->width = width;
    output->height = height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    if (halt_info.stoplist)
        check_stoplist();
    if (halt_info.auto_stop)
        create_surface_frame(output);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    destroy_display_output(data);
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
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "mpvpaper");

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

    bool name_ok = (strcmp(output->name, output->state->monitor) == 0) || (strcmp(output->state->monitor, "*") == 0);
    if (name_ok && !output->layer_surface)
        create_layer_surface(output);
    if (!name_ok)
        destroy_display_output(output);
}

static const struct wl_output_listener output_listener = {
    .geometry = nop,
    .mode = nop,
    .done = output_done,
    .scale = nop,
    .name = output_name,
    .description = nop,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
        uint32_t version) {
    (void)version;

    struct wl_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct display_output *output = calloc(1, sizeof(struct display_output));
        output->state = state;
        output->wl_name = name;
        output->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 4);
        wl_output_add_listener(output->wl_output, &output_listener, output);
        wl_list_insert(&state->outputs, &output->link);

    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;

    struct wl_state *state = data;
    struct display_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &state->outputs, link) {
        if (output->wl_name == name) {
            destroy_display_output(output);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void copy_argv(int argc, char *argv[]) {
    halt_info.argv_copy = calloc(argc+1, sizeof(char*));
    if (!halt_info.argv_copy) {
        fprintf(stderr, "Failed to allocate argv copy");
        exit(EXIT_FAILURE);
    }

    for (int i=0; i < argc; i++) {
        halt_info.argv_copy[i] = strdup(argv[i]);
    }
}

static void set_stop_list() {

    char *stop_path = NULL;
    if (asprintf(&stop_path, "%s/.config/mpvpaper/stoplist", getenv("HOME")) < 0) {
        fprintf(stderr, "Failed to create file path for stoplist");
        exit(EXIT_FAILURE);
    }

    FILE *file = fopen(stop_path, "r");
    if (file) {

        // Dynamically realloc for each program added to list
        char app[512];
        halt_info.stoplist = NULL;
        uint i = 0;
        for (i=0; fscanf(file, "%511s", app) != EOF; i++) {
            halt_info.stoplist = realloc(halt_info.stoplist, (i+1) * sizeof(char *));
            if (!halt_info.stoplist) {
                fprintf(stderr, "Failed to reallocate stop list");
                exit(EXIT_FAILURE);
            }
            halt_info.stoplist [i] = strdup(app);
        }
        // Null terminate
        halt_info.stoplist  = realloc(halt_info.stoplist , (i+1) * sizeof(char *));
        halt_info.stoplist [i] = NULL;

        free(stop_path);
        fclose(file);
    }
}

static void parse_command_line(int argc, char **argv, struct wl_state *state) {

    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"verbose", no_argument, NULL, 'v'},
        {"fork", no_argument, NULL, 'f'},
        {"auto-pause", no_argument, NULL, 'p'},
        {"auto-stop", no_argument, NULL, 's'},
        {"slideshow", required_argument, NULL, 'n'},
        {"layer", required_argument, NULL, 'l'},
        {"mpv-options", required_argument, NULL, 'o'},
        {0, 0, 0, 0}
    };

    const char *usage =
        "Usage: mpvpaper-holder <mpvpaper options>\n"
        "Description:\n"
        "mpvpaper-holder acts as a lean gate keeper before mpvpaper can run\n"
        "\n"
        "It's sole purpose is to check if there is:\n"
        "Any program that is running from the stoplist file\n"
        "- Set in \"~/.config/mpvpaper/stoplist\"\n"
        "And if the wallpaper needs to be seen when drawn\n"
        "- Set with \"-s\" or \"--auto-stop\" mpvpaper option\n";


    char opt;
    while ((opt = getopt_long(argc, argv, "hvfpsn:l:o:Z:", long_options, NULL)) != -1) {

        switch (opt) {
            case 'h':
                fprintf(stdout, "%s", usage);
                exit(EXIT_SUCCESS);
            case 's':
                halt_info.auto_stop = 1;
                break;
        }
    }

    // Need at least a output
    if (optind >= argc) {
        fprintf(stderr, "%s", usage);
        exit(EXIT_FAILURE);
    }

    state->monitor = strdup(argv[optind]);
}

int main(int argc, char **argv) {
    struct wl_state state = {0};
    wl_list_init(&state.outputs);

    parse_command_line(argc, argv, &state);
    set_stop_list();
    copy_argv(argc, argv);

    state.display = wl_display_connect(NULL);
    if (!state.display)
        return EXIT_FAILURE;

    struct wl_registry *registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);
    if (state.compositor == NULL || state.layer_shell == NULL) {
        return EXIT_FAILURE;
    }

    // Check outputs
    wl_display_roundtrip(state.display);
    if (wl_list_empty(&state.outputs))
        return EXIT_FAILURE;

    while (wl_display_dispatch(state.display) != -1) {
        // NOP
    }

    struct display_output *output, *tmp_output;
    wl_list_for_each_safe(output, tmp_output, &state.outputs, link) { destroy_display_output(output); }

    return EXIT_SUCCESS;
}
