#include "vulkan_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <mpv/render_vk.h>

#include <cflogprinter.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct vulkan_state {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPhysicalDeviceFeatures2 enabled_features2;
    VkPhysicalDeviceVulkan12Features enabled_features12;
    const char *enabled_extensions[16];
    uint32_t enabled_extension_count;
    int verbose;
};

struct vulkan_output {
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkImage *images;
    VkImageView *views;
    VkSemaphore *render_finished;
    bool *image_initialized;
    uint32_t image_count;
    VkFence image_acquired;
};

static struct vulkan_state vk;

static const char *vk_result_string(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "unknown Vulkan error";
    }
}

static bool has_device_extension(VkPhysicalDevice physical_device, const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &count, NULL) != VK_SUCCESS)
        return false;

    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions)
        return false;

    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &count, extensions) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(extensions[i].extensionName, name) == 0) {
                found = true;
                break;
            }
        }
    }
    free(extensions);
    return found;
}

static bool append_extension_if_available(VkPhysicalDevice physical_device,
                                          const char *name,
                                          bool required)
{
    if (!has_device_extension(physical_device, name)) {
        if (required)
            cflp_error("Required Vulkan device extension is unavailable: %s", name);
        return !required;
    }

    if (vk.enabled_extension_count >= ARRAY_SIZE(vk.enabled_extensions)) {
        cflp_error("Too many Vulkan device extensions");
        return false;
    }

    vk.enabled_extensions[vk.enabled_extension_count++] = name;
    return true;
}

static bool select_physical_device(const char *device_name)
{
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(vk.instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        cflp_error("No Vulkan physical devices found (%s)", vk_result_string(result));
        return false;
    }

    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices)
        return false;
    vkEnumeratePhysicalDevices(vk.instance, &count, devices);

    int best_score = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        if (vk.verbose)
            cflp_info("Vulkan device: %s", properties.deviceName);

        if (device_name && device_name[0] && strcmp(device_name, properties.deviceName) != 0)
            continue;

        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, NULL);
        VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
        if (!queues)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, queues);

        for (uint32_t q = 0; q < queue_count; q++) {
            if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;

            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
            if (device_name && device_name[0])
                score = 1000;
            if (score > best_score) {
                vk.physical_device = devices[i];
                vk.queue_family = q;
                best_score = score;
            }
            break;
        }
        free(queues);
    }

    free(devices);
    if (!vk.physical_device) {
        if (device_name && device_name[0])
            cflp_error("Requested Vulkan device was not found: %s", device_name);
        else
            cflp_error("No Vulkan device with a graphics queue was found");
        return false;
    }

    return true;
}

bool vulkan_backend_init(const char *device_name, int verbose)
{
    memset(&vk, 0, sizeof(vk));
    vk.verbose = verbose;

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mpvpaper",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "libmpv",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };

    const char *instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ARRAY_SIZE(instance_extensions),
        .ppEnabledExtensionNames = instance_extensions,
    };

    VkResult result = vkCreateInstance(&instance_info, NULL, &vk.instance);
    if (result != VK_SUCCESS) {
        cflp_error("Failed to create Vulkan instance: %s", vk_result_string(result));
        return false;
    }

    if (!select_physical_device(device_name))
        goto fail;

    if (!append_extension_if_available(vk.physical_device, VK_KHR_SWAPCHAIN_EXTENSION_NAME, true))
        goto fail;
    append_extension_if_available(vk.physical_device, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, false);
#ifdef VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
    append_extension_if_available(vk.physical_device, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, false);
#endif
#ifdef VK_KHR_VIDEO_QUEUE_EXTENSION_NAME
    append_extension_if_available(vk.physical_device, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, false);
#endif
#ifdef VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME
    append_extension_if_available(vk.physical_device, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, false);
#endif
#ifdef VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
    append_extension_if_available(vk.physical_device, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, false);
#endif
#ifdef VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME
    append_extension_if_available(vk.physical_device, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, false);
#endif

    VkPhysicalDeviceVulkan12Features supported12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceFeatures2 supported2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &supported12,
    };
    vkGetPhysicalDeviceFeatures2(vk.physical_device, &supported2);
    if (!supported12.timelineSemaphore || !supported12.hostQueryReset) {
        cflp_error("Selected device lacks timelineSemaphore or hostQueryReset support");
        goto fail;
    }

    vk.enabled_features12 = (VkPhysicalDeviceVulkan12Features) {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE,
        .hostQueryReset = VK_TRUE,
    };
    vk.enabled_features2 = (VkPhysicalDeviceFeatures2) {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vk.enabled_features12,
    };

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &vk.enabled_features2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = vk.enabled_extension_count,
        .ppEnabledExtensionNames = vk.enabled_extensions,
    };

    result = vkCreateDevice(vk.physical_device, &device_info, NULL, &vk.device);
    if (result != VK_SUCCESS) {
        cflp_error("Failed to create Vulkan device: %s", vk_result_string(result));
        goto fail;
    }
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk.physical_device, &properties);
    if (vk.verbose)
        cflp_success("Vulkan initialized on %s", properties.deviceName);
    return true;

fail:
    vulkan_backend_destroy();
    return false;
}

void vulkan_backend_destroy(void)
{
    if (vk.device) {
        vkDeviceWaitIdle(vk.device);
        vkDestroyDevice(vk.device, NULL);
    }
    if (vk.instance)
        vkDestroyInstance(vk.instance, NULL);
    memset(&vk, 0, sizeof(vk));
}

mpv_render_context *vulkan_backend_create_mpv_context(mpv_handle *mpv,
                                                       struct wl_display *display)
{
    mpv_render_context *context = NULL;
    mpv_vulkan_init_params init = {
        .instance = vk.instance,
        .physical_device = vk.physical_device,
        .device = vk.device,
        .graphics_queue = vk.queue,
        .graphics_queue_family = vk.queue_family,
        .get_instance_proc_addr = vkGetInstanceProcAddr,
        .features = &vk.enabled_features2,
        .extensions = vk.enabled_extensions,
        .num_extensions = vk.enabled_extension_count,
    };
    const char *backend = "gpu-next";
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_WL_DISPLAY, display},
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_VULKAN},
        {MPV_RENDER_PARAM_BACKEND, (void *)backend},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &init},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };

    int error = mpv_render_context_create(&context, mpv, params);
    if (error < 0) {
        cflp_error("Failed to initialize mpv Vulkan context: %s", mpv_error_string(error));
        return NULL;
    }
    return context;
}

static void destroy_swapchain(struct vulkan_output *output)
{
    if (!output)
        return;
    if (vk.device)
        vkDeviceWaitIdle(vk.device);
    for (uint32_t i = 0; i < output->image_count; i++) {
        if (output->render_finished && output->render_finished[i])
            vkDestroySemaphore(vk.device, output->render_finished[i], NULL);
        if (output->views && output->views[i])
            vkDestroyImageView(vk.device, output->views[i], NULL);
    }
    free(output->image_initialized);
    free(output->render_finished);
    free(output->views);
    free(output->images);
    output->image_initialized = NULL;
    output->render_finished = NULL;
    output->views = NULL;
    output->images = NULL;
    output->image_count = 0;
    if (output->swapchain)
        vkDestroySwapchainKHR(vk.device, output->swapchain, NULL);
    output->swapchain = VK_NULL_HANDLE;
}

static bool create_swapchain(struct vulkan_output *output, uint32_t width, uint32_t height)
{
    VkBool32 present_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical_device, vk.queue_family,
                                         output->surface, &present_supported);
    if (!present_supported) {
        cflp_error("Selected Vulkan graphics queue cannot present to this Wayland surface");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, output->surface, &caps) != VK_SUCCESS)
        return false;

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, output->surface, &format_count, NULL);
    VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
    if (!formats)
        return false;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, output->surface, &format_count, formats);

    VkSurfaceFormatKHR selected = formats[0];
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected = formats[i];
            break;
        }
    }
    free(formats);

    VkExtent2D extent = {width, height};
    if (caps.currentExtent.width != UINT32_MAX)
        extent = caps.currentExtent;
    else {
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainKHR old_swapchain = output->swapchain;
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = output->surface,
        .minImageCount = image_count,
        .imageFormat = selected.format,
        .imageColorSpace = selected.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
                            : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    };

    VkSwapchainKHR new_swapchain;
    VkResult result = vkCreateSwapchainKHR(vk.device, &info, NULL, &new_swapchain);
    if (result != VK_SUCCESS) {
        cflp_error("Failed to create Vulkan swapchain: %s", vk_result_string(result));
        return false;
    }

    if (old_swapchain)
        destroy_swapchain(output);
    output->swapchain = new_swapchain;
    output->format = selected.format;
    output->extent = extent;

    vkGetSwapchainImagesKHR(vk.device, output->swapchain, &output->image_count, NULL);
    output->images = calloc(output->image_count, sizeof(*output->images));
    output->views = calloc(output->image_count, sizeof(*output->views));
    output->render_finished = calloc(output->image_count, sizeof(*output->render_finished));
    output->image_initialized = calloc(output->image_count, sizeof(*output->image_initialized));
    if (!output->images || !output->views || !output->render_finished ||
        !output->image_initialized)
        return false;
    vkGetSwapchainImagesKHR(vk.device, output->swapchain, &output->image_count, output->images);

    for (uint32_t i = 0; i < output->image_count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = output->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = output->format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        result = vkCreateImageView(vk.device, &view_info, NULL, &output->views[i]);
        if (result != VK_SUCCESS) {
            cflp_error("Failed to create Vulkan image view: %s", vk_result_string(result));
            return false;
        }

        VkSemaphoreCreateInfo semaphore_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        result = vkCreateSemaphore(vk.device, &semaphore_info, NULL,
                                   &output->render_finished[i]);
        if (result != VK_SUCCESS) {
            cflp_error("Failed to create Vulkan render semaphore: %s",
                       vk_result_string(result));
            return false;
        }
    }
    return true;
}

struct vulkan_output *vulkan_output_create(struct wl_display *display,
                                            struct wl_surface *surface,
                                            uint32_t width,
                                            uint32_t height)
{
    struct vulkan_output *output = calloc(1, sizeof(*output));
    if (!output)
        return NULL;

    VkWaylandSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = display,
        .surface = surface,
    };
    VkResult result = vkCreateWaylandSurfaceKHR(vk.instance, &surface_info, NULL, &output->surface);
    if (result != VK_SUCCESS) {
        cflp_error("Failed to create Vulkan Wayland surface: %s", vk_result_string(result));
        free(output);
        return NULL;
    }

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    result = vkCreateFence(vk.device, &fence_info, NULL, &output->image_acquired);
    if (result != VK_SUCCESS || !create_swapchain(output, width, height)) {
        vulkan_output_destroy(output);
        return NULL;
    }
    return output;
}

bool vulkan_output_resize(struct vulkan_output *output, uint32_t width, uint32_t height)
{
    return output && create_swapchain(output, width, height);
}

bool vulkan_output_render(struct vulkan_output *output,
                          mpv_render_context *render_context)
{
    uint32_t image_index = 0;
    vkResetFences(vk.device, 1, &output->image_acquired);
    VkResult result = vkAcquireNextImageKHR(vk.device, output->swapchain, UINT64_MAX,
                                            VK_NULL_HANDLE, output->image_acquired,
                                            &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        return false;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        cflp_error("Failed to acquire Vulkan swapchain image: %s", vk_result_string(result));
        return false;
    }

    mpv_vulkan_fbo fbo = {
        .image = output->images[image_index],
        .image_view = output->views[image_index],
        .width = output->extent.width,
        .height = output->extent.height,
        .format = output->format,
        .current_layout = output->image_initialized[image_index]
                            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                            : VK_IMAGE_LAYOUT_UNDEFINED,
        .target_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    if (vkWaitForFences(vk.device, 1, &output->image_acquired, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
        cflp_error("Failed waiting for Vulkan swapchain image");
        return false;
    }

    // Let libmpv signal this binary semaphore from the GPU once rendering and
    // the final PRESENT_SRC_KHR layout transition are complete.  Present waits
    // on it directly, avoiding a full CPU/GPU round trip for every frame.
    mpv_vulkan_sync sync = {
        .signal_semaphore = output->render_finished[image_index],
    };
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_VULKAN_FBO, &fbo},
        {MPV_RENDER_PARAM_VULKAN_SYNC, &sync},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &(int){0}},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };

    int error = mpv_render_context_render(render_context, params);
    if (error < 0) {
        cflp_error("Failed to render Vulkan frame with mpv: %s", mpv_error_string(error));
        return false;
    }

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &output->render_finished[image_index],
        .swapchainCount = 1,
        .pSwapchains = &output->swapchain,
        .pImageIndices = &image_index,
    };
    result = vkQueuePresentKHR(vk.queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        return false;
    if (result != VK_SUCCESS) {
        cflp_error("Failed to present Vulkan frame: %s", vk_result_string(result));
        return false;
    }

    output->image_initialized[image_index] = true;
    mpv_render_context_report_swap(render_context);
    return true;
}

void vulkan_output_destroy(struct vulkan_output *output)
{
    if (!output)
        return;
    destroy_swapchain(output);
    if (output->image_acquired)
        vkDestroyFence(vk.device, output->image_acquired, NULL);
    if (output->surface)
        vkDestroySurfaceKHR(vk.instance, output->surface, NULL);
    free(output);
}
