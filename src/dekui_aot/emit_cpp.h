// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace Dynarmic::IR {
class Block;
}

namespace DekuiAot {

class Image;
struct Program;

/// C symbol for the translated block at `descriptor`.
std::string BlockSymbol(std::uint64_t descriptor);

struct EmittedBlock {
    std::string code;                  ///< Function definition.
    std::set<std::uint64_t> callees;   ///< Blocks it tail-calls (need declarations).
};

/// Translates one optimized IR block to a C++ function. Returns false and sets
/// `unsupported` to the first opcode it cannot express; such blocks are left
/// to the runtime's fallback CPU backend.
bool EmitBlock(const Dynarmic::IR::Block& block, const std::set<std::uint64_t>& translated,
               EmittedBlock& out, std::string& unsupported);

struct EmitStats {
    std::size_t emitted = 0;
    std::map<std::string, std::size_t> unsupported;
};

/// Writes `shards` source files, a block table and a CMake-includable source
/// list into `directory`.
EmitStats EmitProgram(const Image& image, const Program& program,
                      const std::filesystem::path& directory, std::size_t shards);

} // namespace DekuiAot
