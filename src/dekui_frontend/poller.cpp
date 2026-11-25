// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "dekui_frontend/bootstrap.h"
#include "dekui_frontend/motion.h"
#include "dekui_frontend/poller.h"
#include "input_common/main.h"
#include "mm3d/controller_autoconfig.h"
#include "mm3d/diagnostics.h"
#include "mm3d/pause_control.h"
#include "mm3d/player_settings.h"
#include "mm3d/player_settings_bridge.h"

namespace Dekui {

Poller::Poller(Core::System& system_, Host& host_, const KeyboardDefaults& keyboard)
    : system{system_}, host{host_} {
    // Before a game loads, so HID creates its motion device through this engine.
    Motion::Init();
    for (std::size_t i = 0; i < keyboard_buttons.size(); ++i) {
        keyboard_buttons[i] = InputCommon::GenerateKeyboardParam(keyboard.buttons[i]);
    }
    for (std::size_t i = 0; i < keyboard_analogs.size(); ++i) {
        const auto& keys = keyboard.analogs[i];
        keyboard_analogs[i] =
            InputCommon::GenerateAnalogParamFromKeys(keys[0], keys[1], keys[2], keys[3], keys[4], .5f);
    }
}

Poller::~Poller() {
    Motion::Shutdown();
}

void Poller::ConfigureController() {
    // Retry hotplug and saved automatic profiles; preserve custom bindings.
    using namespace MM3D::ControllerAutoConfig;
    const auto& current = Settings::values.current_input_profile;
    const auto controllers = InputCommon::GetConnectedControllers();
    auto mapped = Select(current, controllers, keyboard_buttons,
                         keyboard_analogs, InputCommon::GetControllerButtonBinds,
                         InputCommon::GetControllerAnalogBinds);
    if (!mapped) {
        return;
    }
    auto& profiles = Settings::values.input_profiles;
    int index = Settings::values.current_input_profile_index;
    const bool valid_index = index >= 0 && index < int(profiles.size());
    if ((Managed(current, InputCommon::GetControllerButtonBinds,
                 InputCommon::GetControllerAnalogBinds) ||
         Stale(current, controllers)) &&
        valid_index) {
        profiles[index] = std::move(*mapped);
    } else {
        // Retain the keyboard profile and all unrelated profiles.
        if (valid_index) {
            Settings::SaveProfile(index);
        }
        index = int(profiles.size());
        profiles.push_back(std::move(*mapped));
    }
    Settings::LoadProfile(index);
    system.ApplySettings();
    host.PersistConfig();
    LOG_INFO(Frontend, "Automatically configured controller in profile {}", index);
}

void Poller::PollStateShortcuts() {
    using namespace Settings::NativeButton;
    static constexpr std::array<std::size_t, 3> buttons{ZR, Up, Down};
    std::array<bool, 3> held{};
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        // The same bindings the game reads, rebuilt when the profile changes.
        const auto& params = Settings::values.current_input_profile.buttons[buttons[i]];
        if (params != shortcut_params[i]) {
            shortcut_params[i] = params;
            shortcut_buttons[i] = ::Input::CreateDevice<::Input::ButtonDevice>(params);
        }
        held[i] = shortcut_buttons[i] && shortcut_buttons[i]->GetStatus();
    }
    const bool zr = held[0];
    const bool up = held[1] && !shortcut_held[1];
    const bool down = held[2] && !shortcut_held[2];
    shortcut_held = held;
    if (!zr || !system.IsPoweredOn() || host.ModalOpen() || MM3D::overlay_visible.load()) {
        return;
    }
    if (down) {
        host.SaveNewState();
    } else if (up) {
        host.LoadLatestState();
    }
}

void Poller::Tick() {
#ifdef ENABLE_MM3D_GAME_FOLDER
    if (ticks++ % 15 == 0 && !host.ModalOpen() &&
        !Settings::values.use_artic_base_controller.GetValue()) {
        ConfigureController();
    }
#endif
    PollStateShortcuts();
    if (Motion::Tick()) {
        host.PersistConfig();
    }
    using namespace MM3D::PlayerSettings;
    ApplyPending(Settings::values, [&] { system.ApplySettings(); }, [&] { host.PersistConfig(); });
    if (open_textures.exchange(false)) {
        host.OpenTextureFolder(FileUtil::GetUserPath(FileUtil::UserPath::LoadDir) +
                               "textures/0004000000125500");
    }
    if (controls.exchange(false)) {
        host.ShowControls();
    }
    if (save_states.exchange(false)) {
        host.ShowSaveStates();
    }
    const bool texture_binding = textures_shortcut.exchange(false);
    if (fullscreen_shortcut.exchange(false) || texture_binding) {
        host.EditShortcut(texture_binding ? Shortcut::CustomTextures : Shortcut::Fullscreen);
    }
    if (fullscreen.exchange(false)) {
        host.ToggleFullscreenRequested();
    }
    if (about.exchange(false)) {
#ifdef ENABLE_MM3D_GAME_FOLDER
        host.ShowLicenses(LicensesPath());
#else
        host.ShowAbout();
#endif
    }
    if (quit.exchange(false)) {
        host.Quit();
    }
    if (MM3D::step_requested.exchange(false) && system.IsPoweredOn() &&
        system.frame_limiter.IsFrameAdvancing()) {
        for (unsigned i = 0; i < MM3D::inspection_step_count.load(); ++i) {
            system.frame_limiter.AdvanceFrame();
        }
    }
}

} // namespace Dekui
