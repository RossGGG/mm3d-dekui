// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include "common/settings.h"
#include "mm3d/wide_view.h"
#include "mm3d/widescreen.h"
#include "dekui_visionos/Bridge/emu_window_apple.h"

namespace {
class GraphicsContext_Apple final : public Frontend::GraphicsContext {};
} // namespace

EmuWindow_Apple_Vulkan::EmuWindow_Apple_Vulkan(void* metal_layer) : Frontend::EmuWindow(false) {
    window_info.type = Frontend::WindowSystemType::MacOS;
    window_info.render_surface = metal_layer;
    window_info.render_surface_scale = 1.0f;
}

void EmuWindow_Apple_Vulkan::PollEvents() {
    if (!first_frame_shown.exchange(true) && on_first_frame) {
        on_first_frame();
    }
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_Apple_Vulkan::CreateSharedContext() const {
    return std::make_unique<GraphicsContext_Apple>();
}

void EmuWindow_Apple_Vulkan::Resize(unsigned width, unsigned height) {
    if (width == 0 || height == 0) {
        return;
    }
    // The immersive wide view shows the frame on its own surface: render just
    // the top screen, at its aspect, at the internal resolution.
    if (MM3D::WideView::enabled.load()) {
        const unsigned scale =
            std::clamp(unsigned(Settings::values.resolution_factor.GetValue()), 2u, 10u);
        height = 240 * scale;
        const float aspect = MM3D::Widescreen::Active() ? MM3D::Widescreen::Aspect
                                                        : MM3D::Widescreen::NativeAspect;
        width = unsigned(std::lround(height * aspect));
    }
    // Side-by-side stereo lays out each eye in half the width; give each eye
    // the whole view.
    if (Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::SideBySideFull) {
        width *= 2;
    }
    UpdateCurrentFramebufferLayout(width, height);
}
