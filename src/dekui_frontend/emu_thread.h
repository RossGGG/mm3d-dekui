// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Work that must run on the emulation thread, between RunLoop iterations.

#pragma once

#include <memory>

namespace Core {
class System;
}
namespace Service::CFG {
class Module;
}

namespace Dekui {

/// Applies game language changes requested by the settings overlay and saves
/// them to the console configuration. Construct after the game has loaded;
/// call ApplyPending() every loop iteration and once more before Shutdown(),
/// so a language chosen just before quitting is still saved.
class GameLanguage {
public:
    explicit GameLanguage(Core::System& system);
    void ApplyPending();

private:
    std::shared_ptr<Service::CFG::Module> config;
};

} // namespace Dekui
