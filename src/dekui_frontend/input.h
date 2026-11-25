// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Routes host keyboard and pointer input between the settings overlay and the
// emulated console. Frontends translate their native events into these calls.
// Mouse and scroll events go straight to MM3D::Pointer and MM3D::Scroll
// (mm3d/diagnostics.h). Coordinates are in window points, matching the
// framebuffer layout.

#pragma once

#include <set>
#include <string_view>

namespace Frontend {
class EmuWindow;
}

namespace Dekui::Input {

/// Keys the overlay handles itself. Everything else is Key::Other.
enum class Key {
    Other,
    F1,
    F2,
    Backspace,
    Delete,
    Left,
    Right,
    Home,
    End,
    Enter,
    Escape,
    Tab,
    A,
    Z = A + 25,
};

/// Physical modifier keys. On a Mac keyboard, `control` is Control and
/// `command` is Command.
struct Modifiers {
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool command = false;
};

/// F1 toggles the settings overlay; developer builds also use F2.
bool IsOverlayToggleKey(Key key);

/// Handles an overlay toggle key press or release. Returns true when the event
/// belongs to the overlay. A press also lifts any touch held on `window`.
bool OverlayToggleKey(Key key, bool down, bool repeat, Frontend::EmuWindow& window);

/// Keyboard input for text fields in the overlay.
class OverlayKeyboard {
public:
    /// True while an overlay text field has keyboard focus.
    bool Editing(bool modal_open) const;

    /// Returns true when the overlay consumed the event. `native_key` identifies
    /// the key so its release is delivered after focus moves; `text` is the
    /// printable text the press produces, if any.
    bool Key(int native_key, Input::Key key, Modifiers modifiers, bool down, bool repeat,
             std::string_view text, bool modal_open);

private:
    std::set<int> editing_keys;
};

/// A key for the emulated console's keyboard-mapped controls.
void GameKey(int native_key, bool down, bool repeat);

/// A single touch that drives the overlay pointer. The console touchscreen is
/// not reachable in Dekui builds.
class TouchPointer {
public:
    void Begin(float x, float y);
    void Move(float x, float y);
    void End();

private:
    float last_x = 0;
    float last_y = 0;
};

} // namespace Dekui::Input
