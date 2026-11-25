// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>

namespace MM3D::CpuInterpolation {
// Published by the settings overlay; read by the emulation/rendering paths.
inline std::atomic<bool> enabled{false};
inline bool Enabled() {
    return enabled.load();
}
// CPU interpolation runs every vertex shader on the CPU, which is only usable
// with the shader JIT. Cleared by the core when no JIT is available (visionOS
// launched without a JIT enabler); GPU interpolation is used instead.
inline std::atomic<bool> cpu_vertices_supported{true};
}
