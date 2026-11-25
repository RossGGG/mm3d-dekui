// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/file_util.h"
#include "common/settings.h"
#include "dekui_frontend/bootstrap.h"
#ifdef ENABLE_MM3D_GAME_FOLDER
#include <QFileInfo>
#include "mm3d/game_folder.h"
#include "mm3d/game_setup.h"
#endif
#ifdef CITRA_VISIONOS
#include <oaknut/apple_jit26.hpp>
#endif

namespace Dekui {

void InitializeJit() {
#if defined(OAKNUT_APPLE_JIT26)
    oaknut::apple_jit26::initialize();
#endif
}

#ifdef ENABLE_MM3D_GAME_FOLDER
std::optional<SetupError> PrepareGameFolder() {
    using namespace MM3D::GameFolder;
    if (!EnsureRoot()) {
        return SetupError{SetupError::Kind::Fatal, "Dekui setup",
                          "Could not create Dekui's Files folder."};
    }
    if (Rom().isEmpty()) {
        return SetupError{SetupError::Kind::MissingGame, "Game required",
                          MissingGameMessage().toStdString()};
    }
    if (const auto error = Prepare(); !error.isEmpty()) {
        return SetupError{SetupError::Kind::Fatal, "Dekui setup", error.toStdString()};
    }
    if (const auto error = Validate(); !error.isEmpty()) {
        return SetupError{SetupError::Kind::Fatal, "Dekui setup", error.toStdString()};
    }
    FileUtil::SetUserPath(User().toStdString());
    return std::nullopt;
}

std::optional<std::string> ValidateBootPath(const std::string& path) {
    const auto expected = QFileInfo(MM3D::GameFolder::Rom()).canonicalFilePath();
    if (expected.isEmpty() ||
        QFileInfo(QString::fromStdString(path)).canonicalFilePath() != expected) {
        return "Only the supported game in the Dekui folder can be opened in this build.";
    }
    if (const auto error = MM3D::GameFolder::Validate(); !error.isEmpty()) {
        return error.toStdString();
    }
    return std::nullopt;
}

std::string GamePath() {
    return MM3D::GameFolder::Rom().toStdString();
}

std::string LicensesPath() {
    return MM3D::GameFolder::InstallRoot().toStdString() + "/licenses";
}

void ApplyPlayerDefaults() {
    // Player builds use the stable audio path; do not inherit Azahar's
    // audio-stretching default from an existing shared configuration.
    Settings::values.enable_audio_stretching = false;
}

void ApplyBootSettings() {
    auto& values = Settings::values;
    values.resolution_factor.SetGlobal(true);
    values.volume.SetGlobal(true);
    values.use_vsync.SetGlobal(true);
    values.use_disk_shader_cache.SetGlobal(true);
    values.custom_textures.SetGlobal(true);
#ifdef CITRA_TVOS
    // tvOS cannot JIT the software vertex shader. Never inherit the much
    // slower interpreter path from a copied desktop configuration.
    values.use_hw_shader.SetGlobal(true);
    values.use_hw_shader = true;
#endif
    values.swap_screen.SetGlobal(true);
    values.swap_screen = false;
    values.layout_option.SetGlobal(true);
    values.layout_option = Settings::LayoutOption::SingleScreen;
    values.graphics_api.SetGlobal(true);
    values.graphics_api = Settings::GraphicsAPI::Vulkan;
}
#endif

} // namespace Dekui
