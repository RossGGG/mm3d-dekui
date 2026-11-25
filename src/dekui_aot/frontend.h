// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <dynarmic/ir/basic_block.h>

namespace DekuiAot {

class Image;

/// FPSCR of a freshly created 3DS thread (kernel/thread.cpp): round to zero.
constexpr std::uint32_t DefaultFpscr = 0x03C00010;

/// FPSCR mode MM3D runs in once started (default NaN, flush to zero, round to
/// nearest), as recorded by DEKUI_AOT_COVERAGE. Code reached only through
/// pointers is translated for this mode.
constexpr std::uint32_t GameFpscr = 0x03000000;

/// Location descriptor (Dynarmic A32 UniqueHash) for an ARM or Thumb entry.
std::uint64_t Descriptor(std::uint32_t pc, bool thumb, std::uint32_t fpscr = DefaultFpscr);

/// Translates and optimizes one block exactly as Citra's Dynarmic
/// configuration does at runtime (backend/arm64/a32_address_space.cpp
/// GenerateIR, with define_unpredictable_behaviour and no constant memory
/// reads), reading code through MM3D's substitutions.
Dynarmic::IR::Block TranslateBlock(const Image& image, std::uint64_t descriptor);

} // namespace DekuiAot
