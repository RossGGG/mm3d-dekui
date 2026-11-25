// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <string>
#include "common/common_types.h"

/// Records which guest code the CPU backends translate, for planning and
/// validating ahead-of-time recompilation. Enabled by setting
/// DEKUI_AOT_COVERAGE to an output directory; otherwise every call is a single
/// branch on a cached flag.
///
/// Output (rewritten every few seconds and at exit):
///   instructions.txt  one "address mode" line per translated instruction (A/T)
///   blocks.txt        one "address upper" line per translated block entry, where
///                     upper is the high half of Dynarmic's A32 location
///                     descriptor (T, E, IT and FPSCR mode bits)
///   events.txt        guest instruction-cache invalidations and CRO loads
namespace Core::AotCoverage {

bool Enabled();

/// Records into `directory` from now on, unless DEKUI_AOT_COVERAGE already
/// chose one. Builds with AOT code call this so the blocks that still fall
/// back to the interpreter are known. Call before emulation starts.
void EnableIn(const std::string& directory);

/// Called for every instruction a backend translates. `block` is the Dynarmic
/// A32 location descriptor (UniqueHash) the containing translation started at.
void RecordInstruction(bool thumb, u32 pc, u64 block);

/// Builds the descriptor for backends that do not use Dynarmic.
constexpr u64 BlockDescriptor(u32 pc, u32 cpsr, u32 fpscr) {
    const u64 it = ((cpsr >> 25) & 3) | (((cpsr >> 10) & 0x3F) << 2);
    const u64 upper = (fpscr & 0x07F70000) | ((cpsr >> 5) & 1) | (((cpsr >> 9) & 1) << 1) | (it << 8);
    return pc | (upper << 32);
}

/// Guest-visible events that matter for static translation.
void RecordInvalidate(u32 start, std::size_t length);
void RecordInvalidateAll();
void RecordCroLoad(u32 address, u32 size, u32 data_address, u32 data_size);

/// Writes the files now. Also happens automatically.
void Flush();

} // namespace Core::AotCoverage
