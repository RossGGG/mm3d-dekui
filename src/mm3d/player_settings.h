// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
namespace MM3D::PlayerSettings {
// Render thread requests; the frontend applies/saves through its settings API.
inline std::atomic<int> resolution{4}, volume{100}, vsync{1}, cache{1};
inline std::atomic<int> set_resolution{-1}, set_volume{-1}, set_vsync{-1}, set_cache{-1};
inline std::atomic<int> hardware_shaders{0}, set_hardware_shaders{-1};
inline std::atomic<int> async_shaders{0}, set_async_shaders{-1};
inline std::atomic<int> textures{0}, set_textures{-1};
// Stereoscopic 3D: both eyes side by side (frontends that can show one image
// per eye), and the 3DS depth slider, 0-100.
inline std::atomic<int> stereo{0}, set_stereo{-1};
inline std::atomic<int> depth_3d{100}, set_depth_3d{-1};
inline std::atomic<bool> open_textures{false};
// Frontend signals after the shader/loading screen has faded away.
inline std::atomic<bool> startup_hint_requested{false};
// Native CFG language IDs; requests are applied on the emulation thread.
inline std::atomic<int> game_language{1}, set_game_language{-1};
inline std::atomic<int> game_language_status{0}; // 0 idle, 1 pending, 2 saved, 3 failed
inline constexpr bool SupportedLanguage(int value){return value==1||value==2||value==5;}
inline std::atomic<bool> fullscreen_shortcut{false}, textures_shortcut{false};
inline std::atomic<bool> controls{false}, fullscreen{false}, quit{false}, about{false};
// Motion controls: Settings::MotionSource (0 off, 1 controller, 2 head
// tracking) and gyroscope sensitivity in percent. Applied by Dekui::Motion.
inline std::atomic<int> motion_source{1}, set_motion_source{-1};
inline std::atomic<int> motion_sensitivity{100}, set_motion_sensitivity{-1};
// Set by frontends that can read the wearer's head pose (visionOS).
inline std::atomic<bool> head_motion_available{false};
// Opens the frontend's save state manager (desktop; visionOS has its own window).
inline std::atomic<bool> save_states{false};
}
