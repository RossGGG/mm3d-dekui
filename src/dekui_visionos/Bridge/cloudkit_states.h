// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/// Save states mirrored through CloudKit, for devices without iCloud Drive
/// (Apple TV). The Apple TV keeps its states in its local states folder; the
/// visionOS app mirrors its iCloud Drive states folder, so states reach the
/// Apple TV from the headset and the desktop app, and back.
///
/// Each state (.cst, with its .jpg or .sbs.jpg thumbnail) is one record in the
/// private database of Dekui's iCloud container. A per-device list of the
/// states already in sync tells a new state from a deleted one, so deletions
/// propagate too.
namespace Dekui::CloudKitStates {

/// Starts syncing the emulator's current states folder (call after
/// CloudStates::Enable, which may move it into iCloud Drive). Does nothing
/// when the device has no iCloud account.
void Start();

/// Syncs again soon: new and deleted states on either side. Any thread.
void Sync();

} // namespace Dekui::CloudKitStates
