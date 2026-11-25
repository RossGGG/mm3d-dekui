// Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <optional>
#include <ctime>
#include <random>
#include <sstream>
#include <cryptopp/hex.h>
#include <fmt/ranges.h>
#include "common/archives.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/swap.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "network/network.h"

namespace Core {

#pragma pack(push, 1)
struct CSTHeader {
    std::array<u8, 4> filetype{};    /// Unique Identifier to check the file type (always "CST"0x1B)
    u64_le program_id{};             /// ID of the ROM being executed. Also called title_id
    std::array<u8, 20> revision{};   /// Git hash of the revision this savestate was created with
    u64_le time{};                   /// The time when this save state was created
    std::array<u8, 20> build_name{}; /// The build name (Canary/Nightly) with the version number
    u32_le zero{};                   /// Should be zero, just in case.
    std::array<u8, 20> build_version{}; /// Latest build version, used as compatibility.
    u32_le zero_2{};                    /// Should be zero, just in case.

    std::array<u8, 168> reserved{}; /// Make heading 256 bytes so it has consistent size
};
static_assert(sizeof(CSTHeader) == 256, "CSTHeader should be 256 bytes");
#pragma pack(pop)

constexpr std::array<u8, 4> header_magic_bytes{{'C', 'S', 'T', 0x1B}};

static std::string GetSaveStatePath(u64 program_id, u64 movie_id, u32 slot) {
    if (movie_id) {
        return fmt::format("{}{:016X}.movie{:016X}.{:02d}.cst",
                           FileUtil::GetUserPath(FileUtil::UserPath::StatesDir), program_id,
                           movie_id, slot);
    } else {
        return fmt::format("{}{:016X}.{:02d}.cst",
                           FileUtil::GetUserPath(FileUtil::UserPath::StatesDir), program_id, slot);
    }
}

static bool ValidateSaveState(const CSTHeader& header, SaveStateInfo& info, u64 program_id,
                              const std::string& path) {
    if (header.filetype != header_magic_bytes) {
        LOG_WARNING(Core, "Invalid save state file {}", path);
        return false;
    }
    info.time = header.time;

    if (header.program_id != program_id) {
        LOG_WARNING(Core, "Save state file isn't for the current game {}", path);
        return false;
    }
    const std::string revision = fmt::format("{:02x}", fmt::join(header.revision, ""));
    const std::string build_name =
        header.zero == 0 ? reinterpret_cast<const char*>(header.build_name.data()) : "";
    const std::string build_version =
        header.zero_2 == 0 ? reinterpret_cast<const char*>(header.build_version.data()) : "";

    if (revision == Common::g_scm_rev) {
        info.status = SaveStateInfo::ValidationStatus::OK;
    } else {
        info.build_name = build_name;
        info.build_version = build_version;

        info.status = Common::g_build_version == info.build_version
                          ? SaveStateInfo::ValidationStatus::RevisionMismatch
                          : SaveStateInfo::ValidationStatus::BuildMismatch;
    }
    return true;
}

std::vector<SaveStateInfo> ListSaveStates(u64 program_id, u64 movie_id) {
    std::vector<SaveStateInfo> result;
    result.reserve(SaveStateSlotCount);
    for (u32 slot = 0; slot <= SaveStateSlotCount; ++slot) {
        const auto path = GetSaveStatePath(program_id, movie_id, slot);
        if (!FileUtil::Exists(path)) {
            continue;
        }

        SaveStateInfo info;
        info.slot = slot;

        FileUtil::IOFile file(path, "rb");
        if (!file) {
            LOG_ERROR(Core, "Could not open file {}", path);
            continue;
        }
        CSTHeader header;
        if (file.GetSize() < sizeof(header)) {
            LOG_ERROR(Core, "File too small {}", path);
            continue;
        }
        if (file.ReadBytes(&header, sizeof(header)) != sizeof(header)) {
            LOG_ERROR(Core, "Could not read from file {}", path);
            continue;
        }
        if (!ValidateSaveState(header, info, program_id, path)) {
            continue;
        }

        result.emplace_back(std::move(info));
    }
    return result;
}

SaveStateInfo GetSaveStateInfo(u64 program_id, u64 movie_id, u32 slot) {
    SaveStateInfo info{};
    info.slot = std::numeric_limits<u32>::max();

    const auto path = GetSaveStatePath(program_id, movie_id, slot);
    if (!FileUtil::Exists(path)) {
        return info;
    }

    FileUtil::IOFile file(path, "rb");
    if (!file) {
        LOG_ERROR(Core, "Could not open file {}", path);
        return info;
    }
    CSTHeader header;
    if (file.GetSize() < sizeof(header)) {
        LOG_ERROR(Core, "File too small {}", path);
        return info;
    }
    if (file.ReadBytes(&header, sizeof(header)) != sizeof(header)) {
        LOG_ERROR(Core, "Could not read from file {}", path);
        return info;
    }
    if (ValidateSaveState(header, info, program_id, path)) {
        info.slot = slot;
    }
    return info;
}

namespace {

/// The .cst file name prefix of a game's states, without and with a movie.
std::string StatePrefix(u64 program_id, u64 movie_id) {
    return movie_id ? fmt::format("{:016X}.movie{:016X}.", program_id, movie_id)
                    : fmt::format("{:016X}.", program_id);
}

std::optional<SaveStateInfo> ReadStateInfo(const std::string& path, u64 program_id) {
    FileUtil::IOFile file(path, "rb");
    CSTHeader header;
    if (!file || file.GetSize() < sizeof(header) ||
        file.ReadBytes(&header, sizeof(header)) != sizeof(header)) {
        return std::nullopt;
    }
    SaveStateInfo info{};
    info.slot = std::numeric_limits<u32>::max();
    if (!ValidateSaveState(header, info, program_id, path)) {
        return std::nullopt;
    }
    return info;
}

} // namespace

std::vector<SaveStateFile> ListSaveStateFiles(u64 program_id, u64 movie_id) {
    const auto& directory = FileUtil::GetUserPath(FileUtil::UserPath::StatesDir);
    const std::string prefix = StatePrefix(program_id, movie_id);
    std::vector<SaveStateFile> result;
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory, [&](u64*, const std::string& dir, const std::string& name) {
            constexpr std::string_view extension = ".cst";
            if (name.size() <= prefix.size() + extension.size() || !name.starts_with(prefix) ||
                !name.ends_with(extension)) {
                return true;
            }
            const std::string stem =
                name.substr(prefix.size(), name.size() - prefix.size() - extension.size());
            // Movie states share the game's prefix; list them only with their movie.
            if (!movie_id && stem.starts_with("movie")) {
                return true;
            }
            const std::string path =
                dir.ends_with('/') || dir.ends_with('\\') ? dir + name : dir + '/' + name;
            auto info = ReadStateInfo(path, program_id);
            if (!info) {
                return true;
            }
            SaveStateFile file{path, std::nullopt, *info};
            if (stem.size() == 2 && std::isdigit(static_cast<unsigned char>(stem[0])) &&
                std::isdigit(static_cast<unsigned char>(stem[1]))) {
                file.slot = static_cast<u32>(std::stoul(stem));
                file.info.slot = *file.slot;
            }
            result.push_back(std::move(file));
            return true;
        });
    std::sort(result.begin(), result.end(), [](const SaveStateFile& a, const SaveStateFile& b) {
        return a.info.time != b.info.time ? a.info.time > b.info.time : a.path > b.path;
    });
    return result;
}

std::string SaveStateSlotPath(u64 program_id, u64 movie_id, u32 slot) {
    return GetSaveStatePath(program_id, movie_id, slot);
}

std::string NewSaveStatePath(u64 program_id, u64 movie_id) {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc);
    // The random part keeps two devices saving in the same second apart.
    std::random_device random;
    return fmt::format("{}{}{}-{:04x}.cst", FileUtil::GetUserPath(FileUtil::UserPath::StatesDir),
                       StatePrefix(program_id, movie_id), stamp, random() & 0xFFFF);
}

std::string SaveStateThumbnailPath(const std::string& state_path, bool stereo) {
    const std::string base = state_path.ends_with(".cst")
                                 ? state_path.substr(0, state_path.size() - 4)
                                 : state_path;
    return base + (stereo ? ".sbs.jpg" : ".jpg");
}

bool DeleteSaveStateFile(const std::string& state_path) {
    FileUtil::Delete(SaveStateThumbnailPath(state_path, false));
    FileUtil::Delete(SaveStateThumbnailPath(state_path, true));
    return FileUtil::Delete(state_path);
}

void System::SaveState(u32 slot) const {
    SaveStateToFile(GetSaveStatePath(title_id, movie.GetCurrentMovieID(), slot));
}

void System::SaveStateToFile(const std::string& path) const {
    if (app_loader) {
        if (!app_loader->SupportsSaveStates()) {
            throw std::runtime_error("The current app loader doesn't support save states");
        }
    }

    // Compress while serializing: the uncompressed system state is hundreds of
    // MiB, too much to hold at once on devices with a per-app memory limit.
    Common::Compression::ZSTDCompressingBuffer compressor;
    {
        std::ostream sstream{&compressor};
        oarchive oa{sstream};
        oa&* this;
    }
    auto buffer = compressor.Finish();

    if (!FileUtil::CreateFullPath(path)) {
        throw std::runtime_error("Could not create path " + path);
    }

    // Write beside the target and rename, so a synced folder (iCloud Drive)
    // never sees a partly written state.
    const std::string temporary = path + ".partial";
    FileUtil::IOFile file(temporary, "wb");
    if (!file) {
        throw std::runtime_error("Could not open file " + temporary);
    }

    CSTHeader header{};
    header.filetype = header_magic_bytes;
    header.program_id = title_id;
    std::string rev_bytes;
    CryptoPP::StringSource ss(Common::g_scm_rev, true,
                              new CryptoPP::HexDecoder(new CryptoPP::StringSink(rev_bytes)));
    std::memcpy(header.revision.data(), rev_bytes.data(), sizeof(header.revision));
    header.time = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    const std::string build_fullname = Common::g_build_fullname;
    std::memset(header.build_name.data(), 0, sizeof(header.build_name));
    std::memcpy(header.build_name.data(), build_fullname.c_str(),
                std::min(build_fullname.length(), sizeof(header.build_name) - 1));

    if (file.WriteBytes(&header, sizeof(header)) != sizeof(header) ||
        file.WriteBytes(buffer.data(), buffer.size()) != buffer.size()) {
        file.Close();
        FileUtil::Delete(temporary);
        throw std::runtime_error("Could not write to file " + path);
    }
    file.Close();
    if (!FileUtil::Rename(temporary, path)) {
        FileUtil::Delete(temporary);
        throw std::runtime_error("Could not replace file " + path);
    }
}

void System::LoadState(u32 slot) {
    LoadStateFromFile(GetSaveStatePath(title_id, movie.GetCurrentMovieID(), slot));
}

void System::LoadStateFromFile(const std::string& path) {
    if (app_loader) {
        if (!app_loader->SupportsSaveStates()) {
            throw std::runtime_error("The current app loader doesn't support save states");
        }
    }
    auto room_member = Network::GetRoomMember().lock();
    if (room_member && room_member->IsConnected()) {
        throw std::runtime_error("Unable to load while connected to multiplayer");
    }

    const u64 size = FileUtil::GetSize(path);
    if (size < sizeof(CSTHeader)) {
        throw std::runtime_error("Missing or truncated save state " + path);
    }
    std::vector<u8> buffer(size - sizeof(CSTHeader));
    {

        FileUtil::IOFile file(path, "rb");

        // load header
        CSTHeader header;
        if (file.ReadBytes(&header, sizeof(header)) != sizeof(header)) {
            throw std::runtime_error("Could not read from file at " + path);
        }

        // validate header
        SaveStateInfo info{};
        if (!ValidateSaveState(header, info, title_id, path) ||
            info.status == SaveStateInfo::ValidationStatus::BuildMismatch) {
            throw std::runtime_error("Invalid savestate");
        }

        if (file.ReadBytes(buffer.data(), buffer.size()) != buffer.size()) {
            throw std::runtime_error("Could not read from file at " + path);
        }
    }
    // Decompress while deserializing, without holding the whole state.
    Common::Compression::ZSTDDecompressingBuffer state_buffer{buffer};
    std::istream sstream{&state_buffer};

    // Deserialize
    iarchive ia{sstream};
    ia&* this;
}

std::vector<u8> System::SaveStateBuffer() const {
    // Compress while serializing: the uncompressed system state is hundreds of
    // MiB, too much to hold at once on devices with a per-app memory limit.
    Common::Compression::ZSTDCompressingBuffer compressor;
    {
        std::ostream sstream{&compressor};
        oarchive oa{sstream};
        oa&* this;
    }
    auto buffer = compressor.Finish();

    CSTHeader header{};
    header.filetype = header_magic_bytes;
    header.program_id = title_id;
    std::string rev_bytes;
    CryptoPP::StringSource ss(Common::g_scm_rev, true,
                              new CryptoPP::HexDecoder(new CryptoPP::StringSink(rev_bytes)));
    std::memcpy(header.revision.data(), rev_bytes.data(), sizeof(header.revision));
    header.time = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    const std::string build_fullname = Common::g_build_fullname;
    std::memset(header.build_name.data(), 0, sizeof(header.build_name));
    std::memcpy(header.build_name.data(), build_fullname.c_str(),
                std::min(build_fullname.length(), sizeof(header.build_name) - 1));

    const std::string build_version = Common::g_build_version;
    std::memset(header.build_version.data(), 0, sizeof(header.build_version));
    std::memcpy(header.build_version.data(), build_version.c_str(),
                std::min(build_version.length(), sizeof(header.build_version) - 1));

    std::vector<u8> result((u8*)&header, (u8*)&header + sizeof(header));
    std::copy(buffer.begin(), buffer.end(), std::back_inserter(result));

    return result;
}

bool System::LoadStateBuffer(std::vector<u8> buffer) {
    CSTHeader header;

    if (buffer.size() < sizeof(header)) {
        LOG_ERROR(Core, "Save state too small");
        return false;
    }

    header = *((CSTHeader*)buffer.data());

    if (header.filetype != header_magic_bytes) {
        LOG_ERROR(Core, "Invalid save state");
        return false;
    }

    if (header.program_id != title_id) {
        LOG_ERROR(Core, "Save state isn't for the current game");
        return false;
    }
    std::string revision = fmt::format("{:02x}", fmt::join(header.revision, ""));
    if (revision != Common::g_scm_rev) {
        LOG_ERROR(Core,
                  "Save state file created from a different revision (core: {}, savestate: {})",
                  Common::g_scm_rev, revision);
        return false;
    }

    // Decompress while deserializing, without holding the whole state.
    Common::Compression::ZSTDDecompressingBuffer state_buffer{
        std::span<const u8>{buffer}.subspan(sizeof(CSTHeader))};
    std::istream sstream{&state_buffer};

    // Deserialize
    iarchive ia{sstream};
    ia&* this;

    return true;
}

} // namespace Core
