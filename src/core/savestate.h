// Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {

struct SaveStateInfo {
    u32 slot;
    u64 time;
    enum class ValidationStatus {
        OK,
        RevisionMismatch,
        BuildMismatch,
    } status;
    std::string build_name;
    std::string build_version;
};

constexpr u32 SaveStateSlotCount = 11; // Maximum count of savestate slots

std::vector<SaveStateInfo> ListSaveStates(u64 program_id, u64 movie_id);

SaveStateInfo GetSaveStateInfo(u64 program_id, u64 movie_id, u32 slot);

/// One save state file of a game: a numbered slot (Citra's menus and hotkeys)
/// or a state with its own file, made by Dekui's Save States windows. Separate
/// files never collide, so states saved on different devices can sync freely.
struct SaveStateFile {
    std::string path;
    std::optional<u32> slot; ///< Set for slot files.
    SaveStateInfo info;
};

/// Every valid state of the game (and movie, if one is active), newest first.
std::vector<SaveStateFile> ListSaveStateFiles(u64 program_id, u64 movie_id);

/// The file of a numbered slot.
std::string SaveStateSlotPath(u64 program_id, u64 movie_id, u32 slot);

/// A new, unique path: <title>[.movie<id>].<UTC yyyymmdd-hhmmss>-<random>.cst
std::string NewSaveStatePath(u64 program_id, u64 movie_id);

/// The thumbnail beside a state: .jpg, or .sbs.jpg for side-by-side 3D.
std::string SaveStateThumbnailPath(const std::string& state_path, bool stereo);

/// Deletes a state and its thumbnails.
bool DeleteSaveStateFile(const std::string& state_path);

} // namespace Core
