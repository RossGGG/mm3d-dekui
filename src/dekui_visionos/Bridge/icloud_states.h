// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/// Save states (.cst and their .jpg thumbnails) kept in Dekui's iCloud Drive
/// container, so they sync between devices signed in to the same account.
namespace Dekui::CloudStates {

/// Moves the emulator's save-state folder into the iCloud container, copying
/// existing local states there first (the newer copy wins). Call off the main
/// thread, after the user folder is set up and before a game starts. Returns
/// false, leaving states local, when iCloud Drive is unavailable.
bool Enable();

/// Starts downloading states that exist only in iCloud so far. Returns
/// immediately; does nothing unless Enable succeeded.
void DownloadMissing();

} // namespace Dekui::CloudStates
