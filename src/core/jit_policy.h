// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>

namespace Core::JitPolicy {

/// Whether the CPU and shader JITs may be used where the platform allows them
/// (on visionOS, only with a JIT enabler attached). Frontends set it from the
/// player's preference; System::Init reads it each time a game starts, so a
/// change applies on the next start or restart. Without the JIT, the CPU
/// interpreter runs ahead-of-time translated code when the build has it.
inline std::atomic<bool> allowed{true};

} // namespace Core::JitPolicy
