// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Startup and boot policy shared by the Dekui frontends (Qt and the native
// visionOS app). Nothing here shows UI; errors are returned to the caller.

#pragma once

#include <optional>
#include <string>

namespace Dekui {

struct SetupError {
    enum class Kind {
        MissingGame, ///< No game in the Dekui folder yet; an instruction, not a failure.
        Fatal,
    };
    Kind kind;
    std::string title;
    std::string message;
};

/// Requests the debugger-prepared JIT arena on visionOS. Call it first, while
/// a JIT enabler's debugger may still be attached. No-op elsewhere.
void InitializeJit();

#ifdef ENABLE_MM3D_GAME_FOLDER
/// Creates the Dekui folder, finds and verifies the game and its prepared
/// setup, then points the emulator's user directory into the folder. On the
/// desktop this may run the graphical setup helper, which needs a
/// QCoreApplication.
std::optional<SetupError> PrepareGameFolder();

/// Error text if `path` is not the verified game, or its setup is no longer valid.
std::optional<std::string> ValidateBootPath(const std::string& path);

std::string GamePath();
std::string LicensesPath();

/// Settings that player builds never inherit from an existing configuration.
void ApplyPlayerDefaults();

/// Settings forced on every boot: global player settings, one screen, Vulkan.
void ApplyBootSettings();
#endif

} // namespace Dekui
