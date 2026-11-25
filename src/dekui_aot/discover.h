// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace DekuiAot {

class Image;

enum class Seed : std::uint8_t {
    Entrypoint,  ///< Process entry.
    Coverage,    ///< Translated at runtime (DEKUI_AOT_COVERAGE blocks.txt).
    Pointer,     ///< Aligned word in the image that points at code.
    Successor,   ///< Direct successor or return address of another block.
};

struct BlockInfo {
    std::uint32_t start = 0;
    std::uint32_t end = 0; ///< Address after the last translated instruction.
    std::size_t instructions = 0;
    Seed seed = Seed::Successor;
    bool indirect_exit = false;  ///< Ends by dispatching on a runtime PC.
    bool interpret = false;      ///< Contains an Interpret terminal.
    bool exception = false;      ///< Raises an exception (undefined/unpredictable).
    std::vector<std::uint64_t> successors;
};

struct Program {
    std::map<std::uint64_t, BlockInfo> blocks;
    std::map<std::string, std::size_t> opcode_counts;
    std::size_t rejected_pointers = 0;
    std::size_t zero_fill_skipped = 0;
};

struct DiscoverOptions {
    std::vector<std::uint64_t> coverage;
    bool scan_pointers = true;
};

/// Finds every block reachable from the seeds by following direct control flow.
Program Discover(const Image& image, const DiscoverOptions& options);

/// Reads blocks.txt written by Core::AotCoverage.
std::vector<std::uint64_t> ReadCoverageBlocks(const std::string& path);

} // namespace DekuiAot
