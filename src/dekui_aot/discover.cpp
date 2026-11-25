// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <deque>
#include <fstream>
#include <sstream>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/microinstruction.h>
#include <dynarmic/ir/opcodes.h>
#include <dynarmic/ir/terminal.h>
#include "dekui_aot/discover.h"
#include "dekui_aot/frontend.h"
#include "dekui_aot/image.h"

namespace DekuiAot {

namespace {

using namespace Dynarmic;

void CollectTerminal(const IR::Terminal& terminal, BlockInfo& info) {
    struct Visitor {
        BlockInfo& info;
        void operator()(const IR::Term::Invalid&) {}
        void operator()(const IR::Term::Interpret& t) {
            info.interpret = true;
            info.successors.push_back(t.next.Value());
        }
        void operator()(const IR::Term::ReturnToDispatch&) { info.indirect_exit = true; }
        void operator()(const IR::Term::LinkBlock& t) { info.successors.push_back(t.next.Value()); }
        void operator()(const IR::Term::LinkBlockFast& t) {
            info.successors.push_back(t.next.Value());
        }
        void operator()(const IR::Term::PopRSBHint&) { info.indirect_exit = true; }
        void operator()(const IR::Term::FastDispatchHint&) { info.indirect_exit = true; }
        void operator()(const IR::Term::If& t) {
            boost::apply_visitor(*this, t.then_);
            boost::apply_visitor(*this, t.else_);
        }
        void operator()(const IR::Term::CheckBit& t) {
            boost::apply_visitor(*this, t.then_);
            boost::apply_visitor(*this, t.else_);
        }
        void operator()(const IR::Term::CheckHalt& t) { boost::apply_visitor(*this, t.else_); }
    } visitor{info};
    boost::apply_visitor(visitor, terminal);
}

BlockInfo Analyze(const IR::Block& block, Program& program) {
    BlockInfo info;
    const A32::LocationDescriptor start{block.Location()};
    const A32::LocationDescriptor end{block.EndLocation()};
    info.start = start.PC();
    info.end = end.PC();
    for (const auto& inst : block) {
        ++program.opcode_counts[IR::GetNameOf(inst.GetOpcode())];
        switch (inst.GetOpcode()) {
        case IR::Opcode::PushRSB:
            // A call's return address becomes a block entry.
            info.successors.push_back(inst.GetArg(0).GetU64());
            break;
        case IR::Opcode::A32BXWritePC:
            if (inst.GetArg(0).IsImmediate()) {
                const u32 target = inst.GetArg(0).GetU32();
                info.successors.push_back(
                    start.SetPC(target & ~1u).SetTFlag(target & 1).SetIT({}).UniqueHash());
            }
            break;
        case IR::Opcode::A32ExceptionRaised:
            info.exception = true;
            break;
        default:
            break;
        }
    }
    if (block.HasConditionFailedLocation())
        info.successors.push_back(block.ConditionFailedLocation().Value());
    CollectTerminal(block.GetTerminal(), info);
    return info;
}

bool InText(const Image& image, u32 address) {
    const auto* segment = image.SegmentAt(address);
    return segment && segment->name == "text";
}

} // namespace

std::vector<std::uint64_t> ReadCoverageBlocks(const std::string& path) {
    std::vector<std::uint64_t> blocks;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::uint32_t pc = 0, upper = 0;
        fields >> std::hex >> pc >> upper;
        if (fields)
            blocks.push_back(pc | (std::uint64_t(upper) << 32));
    }
    return blocks;
}

Program Discover(const Image& image, const DiscoverOptions& options) {
    Program program;
    std::deque<std::pair<std::uint64_t, Seed>> work;
    work.emplace_back(Descriptor(image.Entrypoint(), false), Seed::Entrypoint);
    for (const auto descriptor : options.coverage)
        work.emplace_back(descriptor, Seed::Coverage);

    if (options.scan_pointers) {
        // Any aligned word pointing into .text may be a function pointer, a
        // vtable slot or an address in a literal pool. Odd values are Thumb.
        for (const auto& segment : image.Segments()) {
            for (u32 at = segment.address; at + 4 <= segment.address + segment.size; at += 4) {
                const u32 value = *image.Read32(at);
                if (!InText(image, value & ~1u) || ((value & 1) == 0 && (value & 3) != 0))
                    continue;
                work.emplace_back(Descriptor(value & ~1u, value & 1, GameFpscr), Seed::Pointer);
            }
        }
    }

    while (!work.empty()) {
        const auto [descriptor, seed] = work.front();
        work.pop_front();
        if (program.blocks.contains(descriptor))
            continue;
        const u32 pc = static_cast<u32>(descriptor);
        if (!image.Contains(pc)) {
            continue; // Outside the image (HLE or dynamically mapped); dispatch falls back.
        }
        // Zero fill (.bss, padding) decodes as ANDEQ r0,r0,r0 / Thumb LSLS r0,r0,#0.
        // No real block starts there, and .bss holds data, not code, at runtime.
        if (const auto word = image.Read32(pc & ~3u);
            word && ((descriptor >> 32) & 1 ? ((*word >> ((pc & 2) * 8)) & 0xFFFF) == 0 : *word == 0)) {
            ++program.zero_fill_skipped;
            continue;
        }
        IR::Block block = TranslateBlock(image, descriptor);
        BlockInfo info = Analyze(block, program);
        info.seed = seed;
        const bool thumb = A32::LocationDescriptor{block.Location()}.TFlag();
        info.instructions = (info.end - info.start) / (thumb ? 2 : 4); // Approximate for Thumb.
        // A pointer that lands on something undecodable is data, not code.
        if (seed == Seed::Pointer && info.exception && info.start + 4 >= info.end) {
            ++program.rejected_pointers;
            continue;
        }
        for (const auto successor : info.successors)
            work.emplace_back(successor, Seed::Successor);
        program.blocks.emplace(descriptor, std::move(info));
    }
    return program;
}

} // namespace DekuiAot
