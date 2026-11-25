// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cfg/cfg.h"
#include "dekui_frontend/emu_thread.h"
#include "mm3d/player_settings.h"

namespace Dekui {

GameLanguage::GameLanguage(Core::System& system) : config{Service::CFG::GetModule(system)} {
    MM3D::PlayerSettings::game_language.store(config->GetSystemLanguage());
    MM3D::PlayerSettings::game_language_status.store(0);
}

void GameLanguage::ApplyPending() {
    using namespace MM3D::PlayerSettings;
    const int requested = set_game_language.exchange(-1);
    if (!SupportedLanguage(requested)) {
        return;
    }
    const auto old_language = config->GetSystemLanguage();
    config->SetSystemLanguage(static_cast<Service::CFG::SystemLanguage>(requested));
    if (config->GetSystemLanguage() != requested) {
        config->SetSystemLanguage(old_language);
        game_language_status.store(3);
        game_language.store(old_language);
        return;
    }
    const auto result = config->UpdateConfigNANDSavegame();
    if (!result.IsSuccess()) {
        config->SetSystemLanguage(old_language);
    }
    game_language_status.store(result.IsSuccess() ? 2 : 3);
    game_language.store(config->GetSystemLanguage());
}

} // namespace Dekui
