// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Qt key events in the terms of Dekui's frontend-neutral input routing.

#pragma once

#include <Qt>
#include "dekui_frontend/input.h"

inline Dekui::Input::Key ToDekuiKey(int key) {
    using Dekui::Input::Key;
    switch (key) {
    case Qt::Key_F1:
        return Key::F1;
    case Qt::Key_F2:
        return Key::F2;
    case Qt::Key_Backspace:
        return Key::Backspace;
    case Qt::Key_Delete:
        return Key::Delete;
    case Qt::Key_Left:
        return Key::Left;
    case Qt::Key_Right:
        return Key::Right;
    case Qt::Key_Home:
        return Key::Home;
    case Qt::Key_End:
        return Key::End;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return Key::Enter;
    case Qt::Key_Escape:
        return Key::Escape;
    case Qt::Key_Tab:
        return Key::Tab;
    default:
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            return Key(int(Key::A) + (key - Qt::Key_A));
        }
        return Key::Other;
    }
}

inline Dekui::Input::Modifiers ToDekuiModifiers(Qt::KeyboardModifiers modifiers) {
    Dekui::Input::Modifiers result;
    result.shift = modifiers.testFlag(Qt::ShiftModifier);
    result.alt = modifiers.testFlag(Qt::AltModifier);
#ifdef Q_OS_MAC
    // Qt reports Command as Control and Control as Meta on macOS.
    result.command = modifiers.testFlag(Qt::ControlModifier);
    result.control = modifiers.testFlag(Qt::MetaModifier);
#else
    result.control = modifiers.testFlag(Qt::ControlModifier);
    result.command = modifiers.testFlag(Qt::MetaModifier);
#endif
    return result;
}
