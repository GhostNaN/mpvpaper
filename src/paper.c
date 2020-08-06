/*
 *  Copyright (C) 2019-2020 Scoopta
 *  This file is part of GLPaper
 *  GLPaper is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    GLPaper is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with GLPaper.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <wayland-egl.h>
#include <glad/glad.h>
#include <glad/glad_egl.h>
#include <xdg-output-unstable-v1-client-protocol.h>
#include <wlr-layer-shell-unstable-v1-client-protocol.h>

static const char* monitor;
static struct node* output = NULL;
static struct wl_list outputs;
static struct wl_compositor* comp;
static struct zwlr_layer_shell_v1* shell;
static struct zxdg_output_manager_v1* output_manager;

struct node {
    struct wl_output* output;
    int32_t width, height;
    struct wl_list link;
};

static void nop() {}

static void add_interface(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    (void) data;
    if(strcmp(interface, wl_output_interface.name) == 0) {
        struct node* node = malloc(sizeof(struct node));
        node->output = wl_registry_bind(registry, name, &wl_output_interface, version);
        wl_list_insert(&outputs, &node->link);
    } else if(strcmp(interface, wl_compositor_interface.name) == 0) {
        comp = wl_registry_bind(registry, name, &wl_compositor_interface, version);
    } else if(strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, version);
    } else if(strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version);
    }
}

static void get_name(void* data, struct zxdg_output_v1* xdg_output, const char* name) {
    (void) xdg_output;
    struct node* node = data;
    if(strcmp(name, monitor) == 0) {
        output = node;
    }
}

static void config_surface(void* data, struct zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height) {
    (void) data;
    (void) width;
    (void) height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    zwlr_layer_surface_v1_set_exclusive_zone(surface, -1);
    zwlr_layer_surface_v1_set_size(surface, output->width, output->height);
}

static void get_res(void* data, struct wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void) output;
    (void) refresh;
    if((flags & WL_OUTPUT_MODE_CURRENT) == WL_OUTPUT_MODE_CURRENT) {
        struct node* node = data;
        node->width = width;
        node->height = height;
    }
}

static void *get_proc_address_mpv(void *ctx, const char *name){
    (void) ctx;
    return eglGetProcAddress(name);
}

int paper_init(char* _monitor, char* video_path, char* layer_name) {
	monitor = _monitor;
    wl_list_init(&outputs);
    struct wl_display* wl = wl_display_connect(NULL);


    struct wl_registry* registry = wl_display_get_registry(wl);
    struct wl_registry_listener reg_listener = {
        .global = add_interface,
        .global_remove = nop
    };
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(wl);


    struct node* node;
    wl_list_for_each(node, &outputs, link) {
        struct zxdg_output_v1* xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, node->output);
        struct zxdg_output_v1_listener xdg_listener = {
            .description = nop,
            .done = nop,
            .logical_position = nop,
            .logical_size = nop,
            .name = get_name
        };
        zxdg_output_v1_add_listener(xdg_output, &xdg_listener, node);

        struct wl_output_listener out_listener = {
            .done = nop,
            .geometry = nop,
            .mode = get_res,
            .scale = nop
        };
        wl_output_add_listener(node->output, &out_listener, node);
    }
    wl_display_roundtrip(wl);

    if(output == NULL) {
        fprintf(stderr, ":/ sorry about this but we can't seem to find that output\n");
        return 1;
    }


    struct wl_surface* wl_surface = wl_compositor_create_surface(comp);
    struct wl_region* input_region = wl_compositor_create_region(comp);
    struct wl_region* render_region = wl_compositor_create_region(comp);
    wl_region_add(render_region, 0, 0, output->width, output->height);
    wl_surface_set_opaque_region(wl_surface, render_region);
    wl_surface_set_input_region(wl_surface, input_region);
    wl_display_roundtrip(wl);

    enum zwlr_layer_shell_v1_layer layer;

    if(layer_name == NULL) {
        layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    } else if(strcasecmp(layer_name, "top") == 0) {
        layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    } else if(strcasecmp(layer_name, "bottom") == 0) {
        layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
    } else if(strcasecmp(layer_name, "background") == 0) {
        layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    } else if(strcasecmp(layer_name, "overlay") == 0) {
        layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    } else {
        layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    }
    struct zwlr_layer_surface_v1* surface = zwlr_layer_shell_v1_get_layer_surface(shell, wl_surface, output->output, layer, "mpvpaper");
    struct zwlr_layer_surface_v1_listener surface_listener = {
        .closed = nop,
        .configure = config_surface
    };
    zwlr_layer_surface_v1_add_listener(surface, &surface_listener, NULL);
    wl_surface_commit(wl_surface);
    wl_display_roundtrip(wl);

    // Start EGL
    struct wl_egl_window* window = wl_egl_window_create(wl_surface, output->width, output->height);
    eglBindAPI(EGL_OPENGL_API);
    EGLDisplay egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl, NULL);
    eglInitialize(egl_display, NULL, NULL);
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

    // Check for OpenGL combatiblity for creating egl context
    static const struct { int major, minor; } gl_versions[] = {
        {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0},
        {3, 3}, {3, 2}, {3, 1}, {3, 0},
        {0, 0}
    };
    EGLContext egl_ctx = NULL;
    for (int i = 0; gl_versions[i].major > 0; i++) {
        const EGLint ctx_attrib[] = {
            EGL_CONTEXT_MAJOR_VERSION, gl_versions[i].major,
            EGL_CONTEXT_MINOR_VERSION, gl_versions[i].major,
            EGL_NONE
        };
        egl_ctx = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attrib);
        if (egl_ctx) {
            printf("OpenGL %i.%i context loaded\n", gl_versions[i].major, gl_versions[i].minor);
            break;
        }
    }
    if (!egl_ctx) {
        printf("Failed to create EGL context\n");
        return 1;
    }

    EGLSurface egl_surface = eglCreatePlatformWindowSurface(egl_display, config, window, NULL);
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_ctx);

    gladLoadGL();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glViewport(0, 0, output->width, output->height);

    // Start mpv
    mpv_handle* mpv = mpv_create();
    if (!mpv) {
        printf("Failed creating mpv context\n");
        return 1;
    }

    // Set mpv_options passed
    mpv_set_option_string(mpv, "include", "/tmp/mpvpaper.conf");
    remove("/tmp/mpvpaper.conf");

    if (mpv_initialize(mpv) < 0) {
         printf("mpv init failed");
    }

    // Have mpv render onto egl context
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &(mpv_opengl_init_params){
            .get_proc_address = get_proc_address_mpv,
        }},
        // {MPV_RENDER_PARAM_ADVANCED_CONTROL, &(int){1}}
    };
    mpv_render_context *mpv_gl;
    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        printf("Failed to initialize mpv GL context");

    // Play this file.
    const char* cmd[] = {"loadfile", video_path, NULL};
    mpv_command_async(mpv, 0, cmd);

    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &(mpv_opengl_fbo){
            .fbo = 0,
            .w = output->width,
            .h = output->height,
        }},
        // Flip rendering (needed due to flipped GL coordinate system).
        {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
    };

    // Main loop
    while (1) {
        // Closes when the compositor goes away
        if(wl_display_flush(wl) == -1) {
            break;
        }
        // Render next frame
        mpv_render_context_render(mpv_gl, render_params);
        eglSwapBuffers(egl_display, egl_surface);

        mpv_event* event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_SHUTDOWN || event->event_id == MPV_EVENT_IDLE)
            break;
    }

    mpv_render_context_free(mpv_gl);
    mpv_detach_destroy(mpv);

    return 0;
}
