#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <mpv/client.h>
#include <mpv/render.h>

struct vulkan_output;

bool vulkan_backend_init(const char *device_name, int verbose);
void vulkan_backend_destroy(void);
mpv_render_context *vulkan_backend_create_mpv_context(mpv_handle *mpv,
                                                       struct wl_display *display);
struct vulkan_output *vulkan_output_create(struct wl_display *display,
                                            struct wl_surface *surface,
                                            uint32_t width,
                                            uint32_t height);
bool vulkan_output_resize(struct vulkan_output *output,
                          uint32_t width,
                          uint32_t height);
bool vulkan_output_render(struct vulkan_output *output,
                          mpv_render_context *render_context);
void vulkan_output_destroy(struct vulkan_output *output);
