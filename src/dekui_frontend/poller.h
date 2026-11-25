// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The frontend's periodic work: controller auto-configuration, settings
// requested by the overlay, and the overlay's requests for frontend UI.

#pragma once

#include <array>
#include <memory>
#include <string>
#include "common/settings.h"
#include "core/frontend/input.h"

namespace Core {
class System;
}

namespace Dekui {

/// Keyboard bindings for a fresh profile, as codes of the frontend's keyboard
/// engine (Qt key codes in the Qt frontend).
struct KeyboardDefaults {
    std::array<int, Settings::NativeButton::NumButtons> buttons;
    std::array<std::array<int, 5>, Settings::NativeAnalog::NumAnalogs> analogs;
};

enum class Shortcut { Fullscreen, CustomTextures };

/// Frontend UI the overlay can ask for. Called on the UI thread from Poller::Tick().
class Host {
public:
    virtual ~Host() = default;
    /// Automatic controller configuration waits while a modal window is open.
    virtual bool ModalOpen() const = 0;
    virtual void PersistConfig() = 0;
    /// The custom texture folder; it may not exist yet.
    virtual void OpenTextureFolder(const std::string& path) = 0;
    virtual void ShowLicenses(const std::string& path) = 0;
    virtual void ShowAbout() = 0;
    virtual void ShowControls() = 0;
    /// Frontends with their own save state UI (visionOS) need not handle this.
    virtual void ShowSaveStates() {}
    /// Controller shortcuts while the game runs: hold ZR and press D-Pad Down
    /// to save a new state, D-Pad Up to load the newest one.
    virtual void SaveNewState() {}
    virtual void LoadLatestState() {}
    virtual void EditShortcut(Shortcut shortcut) = 0;
    virtual void ToggleFullscreenRequested() = 0;
    /// The overlay's Quit button already asked for confirmation.
    virtual void Quit() = 0;
};

class Poller {
public:
    Poller(Core::System& system, Host& host, const KeyboardDefaults& keyboard);
    ~Poller();
    /// Call about every 16 ms on the UI thread.
    void Tick();

private:
    void ConfigureController();
    void PollStateShortcuts();

    Core::System& system;
    Host& host;
    std::array<std::string, Settings::NativeButton::NumButtons> keyboard_buttons;
    std::array<std::string, Settings::NativeAnalog::NumAnalogs> keyboard_analogs;
    unsigned ticks = 0;
    /// ZR, D-Pad Up and D-Pad Down, from the current input profile.
    std::array<std::string, 3> shortcut_params;
    std::array<std::unique_ptr<::Input::ButtonDevice>, 3> shortcut_buttons;
    std::array<bool, 3> shortcut_held{};
};

} // namespace Dekui
