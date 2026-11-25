// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dekui_visionos/Bridge/stereo_frames.h"

#include <algorithm>
#include "common/hitch_trace.h"
#include "common/logging/log.h"
#include "mm3d/diagnostics.h"

namespace {

MTLPixelFormat SRGBFormat(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatBGRA8Unorm:
        return MTLPixelFormatBGRA8Unorm_sRGB;
    case MTLPixelFormatRGBA8Unorm:
        return MTLPixelFormatRGBA8Unorm_sRGB;
    default:
        return format;
    }
}

} // namespace

void StereoFrames::Deliver(id<MTLTexture> source, uint32_t eyes,
                           const MM3D::WideView::Camera& camera) {
    int slot = -1;
    uint64_t sequence, copy_generation;
    {
        std::unique_lock lock{mutex};
        const auto find = [&] {
            for (int i = 0; i < int(ring.size()); ++i) {
                if (i != newest && i != reading && !ring[i].copying) {
                    slot = i;
                    return true;
                }
            }
            return false;
        };
        if (!find()) {
            // Every spare slot is still being copied into: the GPU is far behind.
            const Common::HitchTrace::Scope hitch_scope{Common::HitchTrace::Phase::DeliverBlit};
            copied.wait(lock, find);
        }
        ring[slot].copying = true;
        sequence = ++delivered;
        copy_generation = generation;
    }
    Slot& target = ring[slot];
    if (!target.texture || target.texture.width != source.width ||
        target.texture.height != source.height) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:SRGBFormat(source.pixelFormat)
                                         width:source.width
                                        height:source.height
                                     mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        descriptor.storageMode = MTLStorageModePrivate;
        target.texture = [source.device newTextureWithDescriptor:descriptor];
        target.copy_target = [target.texture newTextureViewWithPixelFormat:source.pixelFormat];
    }
    if (!queue) {
        queue = [source.device newCommandQueue];
        queue.label = @"Dekui stereo frames";
    }
    // The wide view's HUD layer, valid only during this call.
    id<MTLTexture> hud_source = (__bridge id<MTLTexture>)camera.hud_texture;
    if (hud_source && (!target.hud || target.hud.width != hud_source.width ||
                       target.hud.height != hud_source.height)) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:SRGBFormat(hud_source.pixelFormat)
                                         width:hud_source.width
                                        height:hud_source.height
                                     mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        descriptor.storageMode = MTLStorageModePrivate;
        target.hud = [source.device newTextureWithDescriptor:descriptor];
        target.hud_copy_target = [target.hud newTextureViewWithPixelFormat:hud_source.pixelFormat];
    }
    id<MTLTexture> host_hud_source = (__bridge id<MTLTexture>)camera.host_hud_texture;
    if (host_hud_source &&
        (!target.host_hud || target.host_hud.width != host_hud_source.width ||
         target.host_hud.height != host_hud_source.height)) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:SRGBFormat(host_hud_source.pixelFormat)
                                         width:host_hud_source.width
                                        height:host_hud_source.height
                                     mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        descriptor.storageMode = MTLStorageModePrivate;
        target.host_hud = [source.device newTextureWithDescriptor:descriptor];
        target.host_hud_copy_target =
            [target.host_hud newTextureViewWithPixelFormat:host_hud_source.pixelFormat];
    }
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:source toTexture:target.copy_target];
    if (hud_source) {
        [blit copyFromTexture:hud_source toTexture:target.hud_copy_target];
    }
    if (host_hud_source) {
        [blit copyFromTexture:host_hud_source toTexture:target.host_hud_copy_target];
    }
    // Bounded on-device evidence that Game Options reaches the texture handed
    // to RealityKit. Delay past ImGui's first hidden sizing frame. This runs
    // at most three times per launch, not continuously during gameplay.
    const bool options_visible = MM3D::overlay_visible.load();
    if (options_visible && !options_were_visible && options_readbacks < 3)
        options_readback_delay = 4;
    options_were_visible = options_visible;
    id<MTLBuffer> options_pixels = nil;
    NSUInteger options_row_bytes = 0;
    if (options_visible && options_readback_delay && --options_readback_delay == 0) {
        ++options_readbacks;
        if (host_hud_source) {
            options_row_bytes = (host_hud_source.width * 4 + 255) & ~NSUInteger(255);
            options_pixels = [source.device newBufferWithLength:options_row_bytes * host_hud_source.height
                                                        options:MTLResourceStorageModeShared];
            if (options_pixels)
                [blit copyFromTexture:host_hud_source sourceSlice:0 sourceLevel:0
                        sourceOrigin:MTLOriginMake(0,0,0)
                          sourceSize:MTLSizeMake(host_hud_source.width,host_hud_source.height,1)
                            toBuffer:options_pixels destinationOffset:0
                   destinationBytesPerRow:options_row_bytes
                 destinationBytesPerImage:options_row_bytes * host_hud_source.height];
        } else {
            LOG_WARNING(Frontend, "Immersive Game Options: host HUD texture is missing");
        }
    }
    [blit endEncoding];
    target.eyes = eyes;
    target.camera = camera;
    target.camera.hud_texture = nullptr; // Not valid after delivery.
    target.camera.host_hud_texture = nullptr;
    target.has_hud = hud_source != nil;
    target.has_host_hud = host_hud_source != nil;
    // Publish the frame when the copy completes, instead of waiting for it here:
    // the wait held the presentation thread (and so the emulator's next free
    // frame) for 7-21 ms per frame on Vision Pro. The renderer waits only if it
    // reuses a source before its copy has finished (WaitForReads).
    [command addCompletedHandler:^(id<MTLCommandBuffer> done) {
      std::scoped_lock lock{mutex};
      ring[slot].copying = false;
      std::erase_if(copies, [done](const Copy& copy) { return copy.command == done; });
      if (copy_generation == generation && sequence > published) {
          published = sequence;
          newest = slot;
          taken = false;
      }
      copied.notify_all();
    }];
    {
        // Listed and committed together: WaitForReads must never see an
        // uncommitted command buffer, nor the handler run before the listing.
        std::scoped_lock lock{mutex};
        copies.push_back({command, {source, hud_source, host_hud_source}});
        [command commit];
    }
    if (options_pixels) {
        [command waitUntilCompleted];
        const auto* bytes = static_cast<const uint8_t*>(options_pixels.contents);
        NSUInteger covered = 0;
        for (NSUInteger y=0; y<host_hud_source.height; ++y)
            for (NSUInteger x=0; x<host_hud_source.width; ++x)
                covered += bytes[y*options_row_bytes+x*4+3] != 0;
        LOG_INFO(Frontend, "Immersive Game Options: host HUD {}x{} eyes={} covered_pixels={} native_HUD={}",
                 host_hud_source.width, host_hud_source.height, eyes, covered, hud_source != nil);
    }
}

void StereoFrames::WaitForReads(id<MTLTexture> texture) {
    std::vector<id<MTLCommandBuffer>> pending;
    {
        std::scoped_lock lock{mutex};
        for (const Copy& copy : copies) {
            if (!texture || std::find(copy.sources.begin(), copy.sources.end(), texture) !=
                                copy.sources.end()) {
                pending.push_back(copy.command);
            }
        }
    }
    if (pending.empty()) {
        return;
    }
    const Common::HitchTrace::Scope hitch_scope{Common::HitchTrace::Phase::DeliverBlit};
    for (id<MTLCommandBuffer> command : pending) {
        [command waitUntilCompleted];
    }
}

bool StereoFrames::Acquire(Frame& frame) {
    std::scoped_lock lock{mutex};
    if (taken || newest < 0 || reading >= 0) {
        return false;
    }
    taken = true;
    reading = newest;
    frame = {ring[reading].texture, ring[reading].eyes, reading, ring[reading].camera,
             ring[reading].has_hud ? ring[reading].hud : nil,
             ring[reading].has_host_hud ? ring[reading].host_hud : nil};
    return true;
}

void StereoFrames::Release(int slot) {
    std::scoped_lock lock{mutex};
    if (reading == slot) {
        reading = -1;
    }
}

void StereoFrames::Reset() {
    std::scoped_lock lock{mutex};
    newest = -1;
    taken = true;
    ++generation;
}
