// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include "video_core/renderer_vulkan/vk_external_present.h"

#ifdef __APPLE__
// Types only: vk_common.h loads every entry point through the dispatcher.
#include <vulkan/vulkan_metal.h>
#endif

namespace Vulkan::ExternalPresent {

namespace {
std::mutex sink_mutex;
Sink current_sink{};
void* current_context{};
ReadWaiter current_waiter{};
std::atomic<bool> active{false};
} // Anonymous namespace

void SetSink(Sink sink, void* context, ReadWaiter read_waiter) {
    std::scoped_lock lock{sink_mutex};
    // The previous sink may still be reading delivered images.
    if (current_waiter) {
        current_waiter(current_context, nullptr);
    }
    current_sink = sink;
    current_context = context;
    current_waiter = read_waiter;
    active.store(sink != nullptr);
}

bool Active() {
    return active.load(std::memory_order_relaxed);
}

bool Deliver(void* metal_texture, u32 width, u32 height, u32 eyes, const void* metadata) {
    std::scoped_lock lock{sink_mutex};
    if (!current_sink || !metal_texture) {
        return false;
    }
    current_sink(current_context, metal_texture, width, height, eyes, metadata);
    return true;
}

void WaitForReads(void* metal_texture) {
    ReadWaiter waiter;
    void* context;
    {
        std::scoped_lock lock{sink_mutex};
        waiter = current_waiter;
        context = current_context;
    }
    if (waiter) {
        waiter(context, metal_texture);
    }
}

#ifdef __APPLE__
const void* ExportCreateInfo() {
    static const VkExportMetalObjectCreateInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
        .pNext = nullptr,
        .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
    };
    return &info;
}

void* MetalTexture(vk::Device device, vk::Image image) {
    static const auto export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT"));
    if (!export_objects || !image) {
        return nullptr;
    }
    VkExportMetalTextureInfoEXT texture_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .pNext = nullptr,
        .image = static_cast<VkImage>(image),
        .imageView = VK_NULL_HANDLE,
        .bufferView = VK_NULL_HANDLE,
        .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        .mtlTexture = nullptr,
    };
    VkExportMetalObjectsInfoEXT objects_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &texture_info,
    };
    export_objects(static_cast<VkDevice>(device), &objects_info);
    return (void*)texture_info.mtlTexture;
}
#else
const void* ExportCreateInfo() {
    return nullptr;
}

void* MetalTexture(vk::Device, vk::Image) {
    return nullptr;
}
#endif

} // namespace Vulkan::ExternalPresent
