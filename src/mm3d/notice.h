// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

/// A short message shown at the bottom of the game for a moment (for example
/// "State saved"), drawn by the overlay like the startup settings hint.
namespace MM3D::Notice {

inline std::mutex mutex;
inline std::string pending;
inline std::atomic<bool> requested{false};

/// Callable from any thread.
inline void Show(std::string text) {
    std::scoped_lock lock{mutex};
    pending = std::move(text);
    requested.store(true);
}

/// The overlay takes the newest message, if there is one.
inline std::optional<std::string> Take() {
    if (!requested.exchange(false)) {
        return std::nullopt;
    }
    std::scoped_lock lock{mutex};
    return pending;
}

} // namespace MM3D::Notice
