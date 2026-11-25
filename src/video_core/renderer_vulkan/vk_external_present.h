// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::ExternalPresent {

/// Receives finished frames in place of the swapchain, for presenters outside
/// Vulkan (the visionOS app shows each eye through RealityKit). Called on the
/// presentation thread after the GPU has finished the frame. `metal_texture`
/// is an id<MTLTexture> that is valid only during the call. `eyes` is 2 when
/// the frame holds the left and right eye side by side. `metadata` is the
/// frame's MM3D::WideView::Camera in MM3D presentation builds, else nullptr.
using Sink = void (*)(void* context, void* metal_texture, u32 width, u32 height, u32 eyes,
                      const void* metadata);
/// Optional: a sink that keeps reading a texture after Deliver returns (an
/// asynchronous GPU copy) blocks here until it has finished reading
/// `metal_texture`, or every texture when it is nullptr.
using ReadWaiter = void (*)(void* context, void* metal_texture);

/// Sends frames to `sink` while it is set; nullptr restores the swapchain.
/// Returns once no frame is being delivered to the previous sink.
void SetSink(Sink sink, void* context, ReadWaiter read_waiter = nullptr);
bool Active();
/// Returns false when no sink is set.
bool Deliver(void* metal_texture, u32 width, u32 height, u32 eyes,
             const void* metadata = nullptr);
/// Called before the renderer writes a delivered image again (nullptr: all).
void WaitForReads(void* metal_texture);

/// Chained into an image's create info so its Metal texture can be exported.
const void* ExportCreateInfo();
/// The image's id<MTLTexture> (VK_EXT_metal_objects), or nullptr.
void* MetalTexture(vk::Device device, vk::Image image);

} // namespace Vulkan::ExternalPresent
