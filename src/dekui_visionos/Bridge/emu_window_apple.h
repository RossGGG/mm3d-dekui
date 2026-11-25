// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include "core/frontend/emu_window.h"

/// A Vulkan render target backed by a CAMetalLayer owned by the SwiftUI view.
/// MoltenVK creates the surface from the layer (vk_platform.cpp, MacOS type).
class EmuWindow_Apple_Vulkan final : public Frontend::EmuWindow {
public:
    explicit EmuWindow_Apple_Vulkan(void* metal_layer);

    /// Called on the emulation thread after each presented frame.
    void PollEvents() override;
    std::unique_ptr<Frontend::GraphicsContext> CreateSharedContext() const override;

    /// Size in points. The layer draws one pixel per point (see GameView);
    /// with stereo 3D, so does each eye.
    void Resize(unsigned width, unsigned height);

    std::function<void()> on_first_frame;

private:
    std::atomic<bool> first_frame_shown{false};
};
