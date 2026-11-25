// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <imgui.h>
#include "core/frontend/emu_window.h"
#include "dekui_frontend/input.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/motion_emu.h"
#include "mm3d/diagnostics.h"
#include "mm3d/input_latency.h"

namespace Dekui::Input {

namespace {
ImGuiKey ToImGui(Key key) {
    switch (key) {
    case Key::Backspace:
        return ImGuiKey_Backspace;
    case Key::Delete:
        return ImGuiKey_Delete;
    case Key::Left:
        return ImGuiKey_LeftArrow;
    case Key::Right:
        return ImGuiKey_RightArrow;
    case Key::Home:
        return ImGuiKey_Home;
    case Key::End:
        return ImGuiKey_End;
    case Key::Enter:
        return ImGuiKey_Enter;
    case Key::Escape:
        return ImGuiKey_Escape;
    case Key::Tab:
        return ImGuiKey_Tab;
    default:
        if (key >= Key::A && key <= Key::Z) {
            return ImGuiKey(ImGuiKey_A + (int(key) - int(Key::A)));
        }
        return ImGuiKey_None;
    }
}

bool Printable(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text.front());
    return first >= 0x20 && first != 0x7f;
}
} // namespace

bool IsOverlayToggleKey(Key key) {
#ifdef ENABLE_MM3D_GAME_FOLDER
    return key == Key::F1;
#else
    return key == Key::F1 || key == Key::F2;
#endif
}

bool OverlayToggleKey(Key key, bool down, bool repeat, Frontend::EmuWindow& window) {
    if (!IsOverlayToggleKey(key) || !MM3D::presentation_available.load()) {
        return false;
    }
    if (down && !repeat) {
        MM3D::ToggleOverlayView(key == Key::F2);
        window.TouchReleased();
        InputCommon::GetMotionEmu()->EndTilt();
    }
    return true;
}

bool OverlayKeyboard::Editing(bool modal_open) const {
    return MM3D::overlay_visible.load() && MM3D::text_input_active.load() && !modal_open;
}

bool OverlayKeyboard::Key(int native_key, Input::Key key, Modifiers modifiers, bool down,
                          bool repeat, std::string_view text, bool modal_open) {
    if (!MM3D::overlay_visible.load()) {
        editing_keys.clear();
        return false;
    }
    // Keep delivering releases of keys pressed while editing, so the overlay
    // never sees a key stuck down after focus leaves the text field.
    if (!Editing(modal_open) && (down || !editing_keys.contains(native_key))) {
        return false;
    }
    if (!repeat) {
        if (down) {
            editing_keys.insert(native_key);
        } else {
            editing_keys.erase(native_key);
        }
    }
    MM3D::KeyInput(ImGuiMod_Shift, modifiers.shift);
    MM3D::KeyInput(ImGuiMod_Ctrl, modifiers.control);
    MM3D::KeyInput(ImGuiMod_Super, modifiers.command);
    MM3D::KeyInput(ImGuiMod_Alt, modifiers.alt);
    if (const auto mapped = ToImGui(key); mapped != ImGuiKey_None && !repeat) {
        MM3D::KeyInput(mapped, down);
    }
    if (down && Printable(text) && !modifiers.control && !modifiers.command) {
        MM3D::TextInput(std::string{text}.c_str());
    }
    return true;
}

void GameKey(int native_key, bool down, bool repeat) {
    if (!repeat) {
        MM3D::InputLatency::Key(native_key, down);
    }
    if (down) {
        InputCommon::GetKeyboard()->PressKey(native_key);
    } else {
        InputCommon::GetKeyboard()->ReleaseKey(native_key);
    }
}

void TouchPointer::Begin(float x, float y) {
    last_x = x;
    last_y = y;
    MM3D::Pointer(x, y, 0, true);
}

void TouchPointer::Move(float x, float y) {
    last_x = x;
    last_y = y;
    MM3D::Pointer(x, y);
}

void TouchPointer::End() {
    MM3D::Pointer(last_x, last_y, 0, false);
}

} // namespace Dekui::Input
