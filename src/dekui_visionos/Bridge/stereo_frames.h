// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#import <Metal/Metal.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>
#include "mm3d/wide_view.h"

/// Frames for the stereo presenter, from Vulkan's presentation thread to the
/// main thread, where RealityKit's textures can be updated. Each frame is
/// copied into one of the ring's textures: one the main thread may be reading,
/// the newest one, and the ones whose copies are still on the GPU.
class StereoFrames {
public:
    struct Frame {
        id<MTLTexture> texture;
        uint32_t eyes;
        int slot;
        MM3D::WideView::Camera camera;
        /// The wide view's HUD layer for this frame, or nil.
        id<MTLTexture> hud;
        /// Display-oriented host UI (minimap and Game Options), or nil.
        id<MTLTexture> host_hud;
    };

    /// Presentation thread: queues a GPU copy of `source` (finished by the GPU)
    /// and returns without waiting for it. The frame becomes the newest once
    /// the copy completes. The renderer calls WaitForReads before it writes
    /// `source` (or the camera's HUD textures) again.
    void Deliver(id<MTLTexture> source, uint32_t eyes, const MM3D::WideView::Camera& camera);

    /// Blocks until no queued copy reads `texture` (nil: until no copy is queued).
    void WaitForReads(id<MTLTexture> texture);

    /// Main thread: the newest frame not taken yet. Call Release once the GPU
    /// has finished reading it.
    bool Acquire(Frame& frame);
    void Release(int slot);

    void Reset();

private:
    struct Slot {
        id<MTLTexture> texture;     // sRGB: the frame holds display-encoded colour
        id<MTLTexture> copy_target; // the same memory in the source's format
        uint32_t eyes = 1;
        MM3D::WideView::Camera camera;
        id<MTLTexture> hud;             // sRGB, like texture
        id<MTLTexture> hud_copy_target; // the same memory in the source's format
        bool has_hud = false;
        id<MTLTexture> host_hud;
        id<MTLTexture> host_hud_copy_target;
        bool has_host_hud = false;
        bool copying = false; // a queued copy writes this slot
    };
    struct Copy {
        id<MTLCommandBuffer> command;
        std::array<id<MTLTexture>, 3> sources;
    };

    std::mutex mutex;
    std::condition_variable copied;
    std::array<Slot, 5> ring; // not "slots": Qt defines it as a macro
    std::vector<Copy> copies; // queued copies, oldest first
    int newest = -1;
    int reading = -1;
    bool taken = true;
    uint64_t generation = 0;          // Reset() drops copies queued before it
    uint64_t delivered = 0, published = 0;
    id<MTLCommandQueue> queue;
    bool options_were_visible = false;
    unsigned options_readback_delay = 0;
    unsigned options_readbacks = 0;
};
