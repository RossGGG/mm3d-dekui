// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_simulator_workarounds.h"

#ifdef CITRA_PRIVATE_TEXEL_BUFFERS

#include <vk_mem_alloc.h>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

PrivateTexelBuffer::PrivateTexelBuffer(const Instance& instance_, u64 size) : instance{instance_} {
    const vk::BufferCreateInfo info{
        .size = size,
        .usage = vk::BufferUsageFlagBits::eUniformTexelBuffer |
                 vk::BufferUsageFlagBits::eTransferDst,
    };
    const auto raw_info = static_cast<VkBufferCreateInfo>(info);
    const VmaAllocationCreateInfo allocation_info{
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    };
    VkBuffer raw_buffer{};
    const VkResult result = vmaCreateBuffer(instance.GetAllocator(), &raw_info, &allocation_info,
                                            &raw_buffer, &allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "Failed to allocate a private texel buffer");
    buffer = raw_buffer;
}

PrivateTexelBuffer::~PrivateTexelBuffer() {
    vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(buffer), allocation);
}

void PrivateTexelBuffer::Upload(Scheduler& scheduler, vk::Buffer source, u64 offset, u64 size) {
    if (size == 0) {
        return;
    }
    scheduler.Record([source, destination = buffer, offset, size](vk::CommandBuffer cmdbuf) {
        const auto stages = vk::PipelineStageFlagBits::eVertexShader |
                            vk::PipelineStageFlagBits::eFragmentShader;
        // Earlier draws in this command buffer may still read the range.
        const vk::BufferMemoryBarrier before{
            .srcAccessMask = vk::AccessFlagBits::eShaderRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = destination,
            .offset = offset,
            .size = size,
        };
        cmdbuf.pipelineBarrier(stages, vk::PipelineStageFlagBits::eTransfer, {}, {}, before, {});
        cmdbuf.copyBuffer(source, destination, vk::BufferCopy{offset, offset, size});
        const vk::BufferMemoryBarrier after{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = destination,
            .offset = offset,
            .size = size,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, stages, {}, {}, after, {});
    });
}

} // namespace Vulkan

#endif
