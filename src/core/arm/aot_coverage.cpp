// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <sstream>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/arm/aot_coverage.h"

namespace Core::AotCoverage {
namespace {

using Clock = std::chrono::steady_clock;

// Thumb is tagged in bit 0, which ARM and Thumb addresses never use.
constexpr u32 Key(bool thumb, u32 pc) {
    return (pc & ~1u) | (thumb ? 1u : 0u);
}

// Read once without touching the logger, which may not exist yet.
std::atomic<bool> enabled = [] {
    const char* path = std::getenv("DEKUI_AOT_COVERAGE");
    return path && *path;
}();
std::string requested_directory; ///< From EnableIn(), when the environment sets none.

struct State {
    std::filesystem::path directory;
    std::mutex mutex;
    std::unordered_set<u32> instructions;
    std::unordered_set<u64> blocks;
    std::vector<std::string> events;
    bool dirty = false;
    Clock::time_point last_flush = Clock::now();

    State() {
        const char* path = std::getenv("DEKUI_AOT_COVERAGE");
        if (path && *path)
            directory = path;
        else if (!requested_directory.empty())
            directory = requested_directory;
        else
            return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            LOG_ERROR(Core_ARM11, "AOT coverage: cannot create {}: {}", directory.string(),
                      error.message());
            directory.clear();
            return;
        }
        instructions.reserve(1 << 20);
        // Accumulate across launches: each play session adds to the last.
        ReadExisting();
        LOG_INFO(Core_ARM11, "AOT coverage: recording to {} ({} blocks so far)", directory.string(),
                 blocks.size());
    }

    void ReadExisting() {
        std::ifstream block_file(directory / "blocks.txt");
        std::string line;
        while (std::getline(block_file, line)) {
            std::istringstream fields(line);
            u32 pc = 0, upper = 0;
            fields >> std::hex >> pc >> upper;
            if (fields)
                blocks.insert(pc | (u64(upper) << 32));
        }
        std::ifstream instruction_file(directory / "instructions.txt");
        while (std::getline(instruction_file, line)) {
            std::istringstream fields(line);
            u32 pc = 0;
            char mode = 'A';
            fields >> std::hex >> pc >> mode;
            if (fields)
                instructions.insert(Key(mode == 'T', pc));
        }
        std::ifstream event_file(directory / "events.txt");
        while (std::getline(event_file, line))
            if (!line.empty())
                events.push_back(line);
    }

    ~State() {
        if (!directory.empty())
            Write();
    }

    static void WriteSet(const std::filesystem::path& path, const std::unordered_set<u32>& set) {
        std::vector<u32> sorted(set.begin(), set.end());
        std::sort(sorted.begin(), sorted.end());
        const auto temporary = std::filesystem::path(path).concat(".tmp");
        {
            std::ofstream out(temporary, std::ios::trunc);
            for (const u32 key : sorted)
                out << fmt::format("{:08x} {}\n", key & ~1u, (key & 1) ? 'T' : 'A');
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
    }

    static void WriteBlocks(const std::filesystem::path& path, const std::unordered_set<u64>& set) {
        std::vector<u64> sorted(set.begin(), set.end());
        std::sort(sorted.begin(), sorted.end(), [](u64 a, u64 b) {
            return std::pair{u32(a), a >> 32} < std::pair{u32(b), b >> 32};
        });
        const auto temporary = std::filesystem::path(path).concat(".tmp");
        {
            std::ofstream out(temporary, std::ios::trunc);
            for (const u64 block : sorted)
                out << fmt::format("{:08x} {:08x}\n", u32(block), u32(block >> 32));
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
    }

    // Caller holds `mutex`.
    void Write() {
        if (directory.empty())
            return;
        WriteSet(directory / "instructions.txt", instructions);
        WriteBlocks(directory / "blocks.txt", blocks);
        std::ofstream out(directory / "events.txt", std::ios::trunc);
        for (const auto& event : events)
            out << event << '\n';
        dirty = false;
        last_flush = Clock::now();
    }

    // Caller holds `mutex`.
    void MaybeWrite() {
        if (dirty && Clock::now() - last_flush > std::chrono::seconds(5))
            Write();
    }
};

State& GetState() {
    static State state;
    return state;
}

void AddEvent(std::string event) {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    LOG_INFO(Core_ARM11, "AOT coverage: {}", event);
    state.events.push_back(std::move(event));
    state.dirty = true;
    state.MaybeWrite();
}

} // namespace

bool Enabled() {
    return enabled;
}

void EnableIn(const std::string& directory) {
    if (enabled)
        return; // DEKUI_AOT_COVERAGE wins.
    requested_directory = directory;
    enabled = true;
}

void RecordInstruction(bool thumb, u32 pc, u64 block) {
    if (!enabled)
        return;
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.dirty |= state.instructions.insert(Key(thumb, pc)).second;
    if (pc == u32(block))
        state.dirty |= state.blocks.insert(block).second;
    state.MaybeWrite();
}

void RecordInvalidate(u32 start, std::size_t length) {
    if (enabled)
        AddEvent(fmt::format("invalidate {:08x} {:x}", start, length));
}

void RecordInvalidateAll() {
    if (enabled)
        AddEvent("invalidate-all");
}

void RecordCroLoad(u32 address, u32 size, u32 data_address, u32 data_size) {
    if (enabled)
        AddEvent(fmt::format("cro {:08x} {:x} data {:08x} {:x}", address, size, data_address,
                             data_size));
}

void Flush() {
    if (!enabled)
        return;
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.Write();
}

} // namespace Core::AotCoverage
