// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <mutex>
#include <fmt/format.h>
#include "video_core/renderer_vulkan/vk_platform.h"
namespace Vulkan { class Instance; }
namespace MM3D {
class Overlay {
public:
    Overlay(const Vulkan::Instance& instance, vk::Format format, unsigned image_count,
            std::mutex* submit_mutex = nullptr);
    ~Overlay(); // caller must drain GPU and join the presentation thread first
    /// `eyes` is 2 for a side-by-side stereo frame: the GUI is laid out for
    /// one eye and drawn into both halves.
    void Draw(vk::CommandBuffer command, vk::Framebuffer framebuffer, unsigned width,
              unsigned height, unsigned eyes = 1, bool immersive_hud = false);
    void Presented();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace MM3D
