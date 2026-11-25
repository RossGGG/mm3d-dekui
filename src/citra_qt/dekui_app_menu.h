// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

namespace Dekui::AppMenu {

/// Adds an item to a top-level macOS menu, creating the menu after the
/// application menu the first time. Dekui hides Azahar's Qt menu bar, so these
/// native menus are the only ones. `key` is a Cocoa key equivalent used with
/// Command+Shift, or empty for none. Call on the main thread once the main
/// window is shown.
void AddItem(const std::string& menu, const std::string& title, const std::string& key,
             std::function<void()> action);

} // namespace Dekui::AppMenu
