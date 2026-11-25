// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Dynarmic IR -> C++. Each IR block becomes one function with the
// DekuiAot::Abi::BlockFn signature. The translation follows Dynarmic's arm64
// backend operation by operation (backend/arm64/emit_arm64*.cpp); the helpers
// it calls live in core/arm/aot/block_support.h.

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/cond.h>
#include <dynarmic/ir/microinstruction.h>
#include <dynarmic/ir/opcodes.h>
#include <dynarmic/ir/terminal.h>
#include <dynarmic/ir/type.h>
#include <fmt/format.h>
#include "dekui_aot/discover.h"
#include "dekui_aot/emit_cpp.h"
#include "dekui_aot/frontend.h"
#include "dekui_aot/image.h"

namespace DekuiAot {

namespace {

using namespace Dynarmic;
using Op = IR::Opcode;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Unsupported {
    std::string what;
};

u32 Upper(const IR::LocationDescriptor& location) {
    return static_cast<u32>(A32::LocationDescriptor{location}.SetSingleStepping(false).UniqueHash() >> 32);
}

class BlockEmitter {
public:
    BlockEmitter(const IR::Block& block, const std::set<u64>& translated, EmittedBlock& out)
        : block(block), translated(translated), out(out) {}

    void Emit() {
        const u64 descriptor = block.Location().Value();
        Raw(fmt::format("// {:08x} {}\n", A32::LocationDescriptor{block.Location()}.PC(),
                        A32::LocationDescriptor{block.Location()}.TFlag() ? "Thumb" : "ARM"));
        Raw(fmt::format("extern \"C\" DEKUI_AOT_BLOCK u64 {}(State* s) {{\n", BlockSymbol(descriptor)));
        if (block.GetCondition() != IR::Cond::AL) {
            Line(fmt::format("if (!ConditionPassed(s->cpsr_nzcv, {})) {{",
                             static_cast<unsigned>(block.GetCondition())));
            ++indent;
            Line(fmt::format("s->ticks -= {};", block.ConditionFailedCycleCount()));
            Terminal(IR::Term::LinkBlock{block.ConditionFailedLocation()});
            --indent;
            Line("}");
        }
        for (const IR::Inst& inst : block)
            Instruction(inst);
        Line(fmt::format("s->ticks -= {};", block.CycleCount()));
        Terminal(block.GetTerminal());
        Raw("}\n\n");
    }

private:
    // ---- Output -------------------------------------------------------------

    void Raw(const std::string& text) { out.code += text; }
    void Line(const std::string& text) {
        out.code.append(4 * indent, ' ');
        out.code += text;
        out.code += '\n';
    }

    static const char* CType(IR::Type type) {
        switch (type) {
        case IR::Type::U1:
            return "bool";
        case IR::Type::U8:
            return "u8";
        case IR::Type::U16:
            return "u16";
        case IR::Type::U32:
        case IR::Type::NZCVFlags:
            return "u32";
        case IR::Type::U64:
            return "u64";
        default:
            throw Unsupported{fmt::format("type {}", static_cast<int>(type))};
        }
    }

    std::string Name(const IR::Inst* inst) {
        const auto it = names.find(inst);
        if (it == names.end())
            throw Unsupported{"use before definition"};
        return it->second;
    }

    std::string Arg(const IR::Inst& inst, std::size_t index) {
        const IR::Value value = inst.GetArg(index);
        if (!value.IsImmediate())
            return Name(value.GetInst());
        switch (value.GetType()) {
        case IR::Type::U1:
            return value.GetU1() ? "true" : "false";
        case IR::Type::U8:
            return fmt::format("u8(0x{:x})", value.GetU8());
        case IR::Type::U16:
            return fmt::format("u16(0x{:x})", value.GetU16());
        case IR::Type::U32:
            return fmt::format("0x{:x}u", value.GetU32());
        case IR::Type::U64:
            return fmt::format("0x{:x}ull", value.GetU64());
        case IR::Type::NZCVFlags:
            // Value::EmptyNZCVImmediateMarker: no flags set (see A32SetCpsrNZC).
            return "0u";
        default:
            throw Unsupported{fmt::format("immediate type {}", static_cast<int>(value.GetType()))};
        }
    }

    u64 ImmU64(const IR::Inst& inst, std::size_t index) {
        const IR::Value value = inst.GetArg(index);
        if (!value.IsImmediate())
            throw Unsupported{"expected immediate"};
        return value.GetImmediateAsU64();
    }

    /// Declares the instruction's result.
    std::string Define(const IR::Inst& inst, const std::string& expression) {
        const std::string name = fmt::format("v{}", names.size());
        names.emplace(&inst, name);
        Line(fmt::format("[[maybe_unused]] const {} {} = {};", CType(inst.GetType()), name, expression));
        return name;
    }

    /// Declares a result whose name is also the stem of helper variables.
    std::string Reserve(const IR::Inst& inst) {
        const std::string name = fmt::format("v{}", names.size());
        names.emplace(&inst, name);
        return name;
    }

    const IR::Inst& Parent(const IR::Inst& pseudo) {
        const IR::Value value = pseudo.GetArg(0);
        if (value.IsImmediate())
            throw Unsupported{"pseudo-operation on immediate"};
        return *value.GetInst();
    }

    // ---- Instructions ---------------------------------------------------------

    void Instruction(const IR::Inst& inst) {
        switch (inst.GetOpcode()) {
        case Op::Void:
        case Op::PushRSB: // The return stack buffer is only a hint.
        case Op::A32InstructionSynchronizationBarrier: // hook_isb is off.
            return;
        case Op::A32DataSynchronizationBarrier:
        case Op::A32DataMemoryBarrier:
            Line("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
            return;

        // Context
        case Op::A32SetCheckBit:
            Line(fmt::format("s->check_bit = {};", Arg(inst, 0)));
            return;
        case Op::A32GetRegister:
            Define(inst, fmt::format("s->regs[{}]", A32::RegNumber(inst.GetArg(0).GetA32RegRef())));
            return;
        case Op::A32SetRegister:
            Line(fmt::format("s->regs[{}] = {};", A32::RegNumber(inst.GetArg(0).GetA32RegRef()),
                             Arg(inst, 1)));
            return;
        case Op::A32GetExtendedRegister32:
            Define(inst, fmt::format("s->ext_regs[{}]", ExtIndex(inst.GetArg(0).GetA32ExtRegRef())));
            return;
        case Op::A32SetExtendedRegister32:
            Line(fmt::format("s->ext_regs[{}] = {};", ExtIndex(inst.GetArg(0).GetA32ExtRegRef()),
                             Arg(inst, 1)));
            return;
        case Op::A32GetExtendedRegister64:
            Define(inst, fmt::format("GetD(s, {})", ExtIndex(inst.GetArg(0).GetA32ExtRegRef())));
            return;
        case Op::A32SetExtendedRegister64:
            Line(fmt::format("SetD(s, {}, {});", ExtIndex(inst.GetArg(0).GetA32ExtRegRef()),
                             Arg(inst, 1)));
            return;
        case Op::A32GetCpsr:
            Define(inst, "GetCpsr(s)");
            return;
        case Op::A32SetCpsr:
            Line(fmt::format("s->SetCpsr({});", Arg(inst, 0)));
            return;
        case Op::A32SetCpsrNZCV:
        case Op::A32SetCpsrNZCVRaw: // Stored unmasked, as the arm64 backend does.
            Line(fmt::format("s->cpsr_nzcv = {};", Arg(inst, 0)));
            return;
        case Op::A32SetCpsrNZCVQ:
            Line(fmt::format("s->cpsr_nzcv = {0} & 0xF0000000u; s->cpsr_q = {0} & 0x08000000u;",
                             Arg(inst, 0)));
            return;
        case Op::A32SetCpsrNZ:
            Line(fmt::format("s->cpsr_nzcv = (s->cpsr_nzcv & 0x30000000u) | {};", Arg(inst, 0)));
            return;
        case Op::A32SetCpsrNZC:
            Line(fmt::format("s->cpsr_nzcv = (s->cpsr_nzcv & 0x10000000u) | {} | ({} ? 0x20000000u : 0u);",
                             Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::A32GetCFlag:
            Define(inst, "(s->cpsr_nzcv >> 29) & 1");
            return;
        case Op::A32OrQFlag:
            Line(fmt::format("s->cpsr_q |= u32({}) << 27;", Arg(inst, 0)));
            return;
        case Op::A32GetGEFlags:
            Define(inst, "s->cpsr_ge");
            return;
        case Op::A32SetGEFlags:
            Line(fmt::format("s->cpsr_ge = {};", Arg(inst, 0)));
            return;
        case Op::A32SetGEFlagsCompressed: {
            const std::string v = Arg(inst, 0);
            Line(fmt::format("s->cpsr_ge = ({0} >> 19 & 1 ? 0xFF000000u : 0u) | ({0} >> 18 & 1 ? 0x00FF0000u : 0u) | "
                             "({0} >> 17 & 1 ? 0x0000FF00u : 0u) | ({0} >> 16 & 1 ? 0x000000FFu : 0u);",
                             v));
            return;
        }
        case Op::A32BXWritePC: {
            const u32 upper_without_t = Upper(block.EndLocation()) & 0xFFFFFFFE;
            const IR::Value target = inst.GetArg(0);
            if (target.IsImmediate()) {
                const u32 pc = target.GetU32();
                const bool thumb = pc & 1;
                Line(fmt::format("s->regs[15] = 0x{:x}u; s->upper_location_descriptor = 0x{:x}u;",
                                 pc & (thumb ? 0xFFFFFFFE : 0xFFFFFFFC), upper_without_t | thumb));
            } else {
                const std::string pc = Arg(inst, 0);
                Line(fmt::format("s->regs[15] = {0} & (({0} & 1) ? 0xFFFFFFFEu : 0xFFFFFFFCu); "
                                 "s->upper_location_descriptor = 0x{1:x}u | ({0} & 1);",
                                 pc, upper_without_t));
            }
            return;
        }
        case Op::A32UpdateUpperLocationDescriptor: {
            for (const IR::Inst& other : block)
                if (other.GetOpcode() == Op::A32BXWritePC)
                    return;
            SetUpper(block.EndLocation(), block.Location());
            return;
        }
        case Op::A32CallSupervisor:
            Line(fmt::format("s->env->call_svc(s, 0x{:x}u);", ImmU64(inst, 0)));
            return;
        case Op::A32ExceptionRaised:
            Line(fmt::format("s->env->exception_raised(s, 0x{:x}u, 0x{:x}u);", ImmU64(inst, 0),
                             ImmU64(inst, 1)));
            return;
        case Op::A32GetFpscr:
            Line("s->fpsr = ReadHostFpsr();");
            Define(inst, "(s->upper_location_descriptor & 0xFFFF0000u) | s->fpsr | s->fpsr_nzcv");
            return;
        case Op::A32SetFpscr:
            Line(fmt::format("s->SetFpscr({}); WriteHostFpsr(s->fpsr);", Arg(inst, 0)));
            return;
        case Op::A32GetFpscrNZCV:
            Define(inst, "s->fpsr_nzcv");
            return;
        case Op::A32SetFpscrNZCV:
            Line(fmt::format("s->fpsr_nzcv = {};", Arg(inst, 0)));
            return;

        // Memory
        case Op::A32ReadMemory8:
            Define(inst, fmt::format("Read8(s, {})", Arg(inst, 1)));
            return;
        case Op::A32ReadMemory16:
            Define(inst, fmt::format("Read16(s, {})", Arg(inst, 1)));
            return;
        case Op::A32ReadMemory32:
            Define(inst, fmt::format("Read32(s, {})", Arg(inst, 1)));
            return;
        case Op::A32ReadMemory64:
            Define(inst, fmt::format("Read64(s, {})", Arg(inst, 1)));
            return;
        case Op::A32WriteMemory8:
            Line(fmt::format("Write8(s, {}, {});", Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::A32WriteMemory16:
            Line(fmt::format("Write16(s, {}, {});", Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::A32WriteMemory32:
            Line(fmt::format("Write32(s, {}, {});", Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::A32WriteMemory64:
            Line(fmt::format("Write64(s, {}, {});", Arg(inst, 1), Arg(inst, 2)));
            return;
        // Exclusive accesses follow emit_arm64_memory.cpp: the local
        // exclusive_state gates the store, the global monitor does the rest.
        case Op::A32ClearExclusive:
            Line("s->exclusive_state = 0;");
            return;
        case Op::A32ExclusiveReadMemory8:
        case Op::A32ExclusiveReadMemory16:
        case Op::A32ExclusiveReadMemory32:
        case Op::A32ExclusiveReadMemory64:
            Line("s->exclusive_state = 1;");
            Define(inst, fmt::format("s->env->exclusive_read{}(s, {})", Bits(inst.GetType()), Arg(inst, 1)));
            return;
        case Op::A32ExclusiveWriteMemory8:
        case Op::A32ExclusiveWriteMemory16:
        case Op::A32ExclusiveWriteMemory32:
        case Op::A32ExclusiveWriteMemory64: {
            const std::string name = Reserve(inst);
            Line(fmt::format("u32 {} = 1;", name));
            Line(fmt::format("if (s->exclusive_state) {{ s->exclusive_state = 0; {} = s->env->exclusive_write{}(s, {}, {}); }}",
                             name, Bits(inst.GetArg(2).GetType()), Arg(inst, 1), Arg(inst, 2)));
            return;
        }

        // Coprocessors
        case Op::A32CoprocInternalOperation:
            Line(fmt::format("s->env->coproc_internal(s, {});", Coproc(inst)));
            return;
        case Op::A32CoprocSendOneWord:
            Line(fmt::format("s->env->coproc_send_one(s, {}, {});", Coproc(inst), Arg(inst, 1)));
            return;
        case Op::A32CoprocSendTwoWords:
            Line(fmt::format("s->env->coproc_send_two(s, {}, {}, {});", Coproc(inst), Arg(inst, 1),
                             Arg(inst, 2)));
            return;
        case Op::A32CoprocGetOneWord:
            Define(inst, fmt::format("s->env->coproc_get_one(s, {})", Coproc(inst)));
            return;
        case Op::A32CoprocGetTwoWords:
            Define(inst, fmt::format("s->env->coproc_get_two(s, {})", Coproc(inst)));
            return;
        case Op::A32CoprocLoadWords:
            Line(fmt::format("s->env->coproc_load(s, {}, {});", Coproc(inst), Arg(inst, 1)));
            return;
        case Op::A32CoprocStoreWords:
            Line(fmt::format("s->env->coproc_store(s, {}, {});", Coproc(inst), Arg(inst, 1)));
            return;

        // Pseudo-operations
        case Op::GetCarryFromOp:
            Define(inst, Carry(Parent(inst)));
            return;
        case Op::GetOverflowFromOp:
            Define(inst, Overflow(Parent(inst)));
            return;
        case Op::GetNZCVFromOp: {
            if (inst.GetArg(0).IsImmediate()) {
                Define(inst, ImmediateNZ(inst));
                return;
            }
            const IR::Inst& parent = Parent(inst);
            if (IsAddSub32(parent))
                Define(inst, Name(&parent) + "_f.nzcv");
            else
                Define(inst, NZ(parent));
            return;
        }
        case Op::GetNZFromOp:
            Define(inst, inst.GetArg(0).IsImmediate() ? ImmediateNZ(inst) : NZ(Parent(inst)));
            return;
        case Op::GetGEFromOp: {
            const IR::Inst& parent = Parent(inst);
            if (!packed_ge.contains(&parent))
                throw Unsupported{"GE from non-packed op"};
            Define(inst, Name(&parent) + "_ge");
            return;
        }
        case Op::GetCFlagFromNZCV:
            Define(inst, fmt::format("({} >> 29) & 1", Arg(inst, 0)));
            return;

        // Integer data processing
        case Op::Pack2x32To1x64:
            Define(inst, fmt::format("u64({}) | (u64({}) << 32)", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::LeastSignificantWord:
            Define(inst, fmt::format("u32({})", Arg(inst, 0)));
            return;
        case Op::MostSignificantWord:
            Define(inst, fmt::format("u32({} >> 32)", Arg(inst, 0)));
            return;
        case Op::LeastSignificantHalf:
            Define(inst, fmt::format("u16({})", Arg(inst, 0)));
            return;
        case Op::LeastSignificantByte:
            Define(inst, fmt::format("u8({})", Arg(inst, 0)));
            return;
        case Op::MostSignificantBit:
            Define(inst, fmt::format("({} >> 31) != 0", Arg(inst, 0)));
            return;
        case Op::IsZero32:
        case Op::IsZero64:
            Define(inst, fmt::format("{} == 0", Arg(inst, 0)));
            return;
        case Op::TestBit:
            Define(inst, fmt::format("(({} >> {}) & 1) != 0", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::LogicalShiftLeft32:
            Define(inst, fmt::format("Lsl({}, {})", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::LogicalShiftRight32:
            Define(inst, fmt::format("Lsr({}, {})", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::ArithmeticShiftRight32:
            Define(inst, fmt::format("Asr({}, {})", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::RotateRight32:
            Define(inst, fmt::format("Ror({}, {})", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::RotateRightExtended:
            Define(inst, fmt::format("(u32({}) << 31) | ({} >> 1)", Arg(inst, 1), Arg(inst, 0)));
            return;
        case Op::LogicalShiftLeft64:
            Define(inst, fmt::format("Lsl64({}, {})", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::LogicalShiftRight64:
            Define(inst, fmt::format("Lsr64({}, {})", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::ArithmeticShiftRight64:
            Define(inst, fmt::format("Asr64({}, {})", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::Add32:
        case Op::Sub32: {
            const std::string name = Reserve(inst);
            const bool sub = inst.GetOpcode() == Op::Sub32;
            Line(fmt::format("const AddResult {}_f = AddWithCarry({}, {}{}, {});", name, Arg(inst, 0),
                             sub ? "~" : "", Arg(inst, 1), Arg(inst, 2)));
            Line(fmt::format("[[maybe_unused]] const u32 {0} = {0}_f.value;", name));
            return;
        }
        case Op::Add64:
            Define(inst, fmt::format("{} + {} + u64({})", Arg(inst, 0), Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::Sub64:
            Define(inst, fmt::format("{} + ~{} + u64({})", Arg(inst, 0), Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::Mul32:
            Define(inst, fmt::format("{} * {}", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::Mul64:
            Define(inst, fmt::format("{} * {}", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::And32:
        case Op::And64:
            Define(inst, fmt::format("{} & {}", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::AndNot32:
        case Op::AndNot64:
            Define(inst, fmt::format("{} & ~{}", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::Eor32:
        case Op::Eor64:
            Define(inst, fmt::format("{} ^ {}", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::Or32:
        case Op::Or64:
            Define(inst, fmt::format("{} | {}", Arg(inst, 0), Arg(inst, 1)));
            return;
        case Op::Not32:
        case Op::Not64:
            Define(inst, fmt::format("~{}", Arg(inst, 0)));
            return;
        case Op::SignExtendByteToWord:
            Define(inst, fmt::format("u32(s32(s8({})))", Arg(inst, 0)));
            return;
        case Op::SignExtendHalfToWord:
            Define(inst, fmt::format("u32(s32(s16({})))", Arg(inst, 0)));
            return;
        case Op::SignExtendWordToLong:
            Define(inst, fmt::format("u64(s64(s32({})))", Arg(inst, 0)));
            return;
        case Op::ZeroExtendByteToWord:
        case Op::ZeroExtendHalfToWord:
            Define(inst, fmt::format("u32({})", Arg(inst, 0)));
            return;
        case Op::ZeroExtendWordToLong:
            Define(inst, fmt::format("u64({})", Arg(inst, 0)));
            return;
        case Op::ByteReverseWord:
            Define(inst, fmt::format("__builtin_bswap32({})", Arg(inst, 0)));
            return;
        case Op::ByteReverseHalf:
            Define(inst, fmt::format("u16(__builtin_bswap16({}))", Arg(inst, 0)));
            return;
        case Op::CountLeadingZeros32:
            Define(inst, fmt::format("Clz({})", Arg(inst, 0)));
            return;
        case Op::UnsignedSaturation:
        case Op::SignedSaturation: {
            const std::string name = Reserve(inst);
            Line(fmt::format("bool {}_q;", name));
            Line(fmt::format("[[maybe_unused]] const u32 {0} = {1}Saturation({2}, {3}, {0}_q);", name,
                             inst.GetOpcode() == Op::SignedSaturation ? "Signed" : "Unsigned",
                             Arg(inst, 0), ImmU64(inst, 1)));
            return;
        }

        // Parallel arithmetic
        case Op::PackedAddU8: return Packed(inst, "u8", "s32", "r = x + y; ge = r >> 8", true);
        case Op::PackedAddS8: return Packed(inst, "s8", "s32", "r = x + y; ge = r >= 0", true);
        case Op::PackedSubU8: return Packed(inst, "u8", "s32", "r = x - y; ge = r >= 0", true);
        case Op::PackedSubS8: return Packed(inst, "s8", "s32", "r = x - y; ge = r >= 0", true);
        case Op::PackedAddU16: return Packed(inst, "u16", "s32", "r = x + y; ge = r >> 16", true);
        case Op::PackedAddS16: return Packed(inst, "s16", "s32", "r = x + y; ge = r >= 0", true);
        case Op::PackedSubU16: return Packed(inst, "u16", "s32", "r = x - y; ge = r >= 0", true);
        case Op::PackedSubS16: return Packed(inst, "s16", "s32", "r = x - y; ge = r >= 0", true);
        case Op::PackedHalvingAddU8: return Packed(inst, "u8", "s32", "r = (x + y) >> 1", false);
        case Op::PackedHalvingAddS8: return Packed(inst, "s8", "s32", "r = (x + y) >> 1", false);
        case Op::PackedHalvingSubU8: return Packed(inst, "u8", "s32", "r = (x - y) >> 1", false);
        case Op::PackedHalvingSubS8: return Packed(inst, "s8", "s32", "r = (x - y) >> 1", false);
        case Op::PackedHalvingAddU16: return Packed(inst, "u16", "s32", "r = (x + y) >> 1", false);
        case Op::PackedHalvingAddS16: return Packed(inst, "s16", "s32", "r = (x + y) >> 1", false);
        case Op::PackedHalvingSubU16: return Packed(inst, "u16", "s32", "r = (x - y) >> 1", false);
        case Op::PackedHalvingSubS16: return Packed(inst, "s16", "s32", "r = (x - y) >> 1", false);
        case Op::PackedSaturatedAddU8: return Packed(inst, "u8", "s32", "r = Saturate<u8>(x + y)", false);
        case Op::PackedSaturatedAddS8: return Packed(inst, "s8", "s32", "r = Saturate<s8>(x + y)", false);
        case Op::PackedSaturatedSubU8: return Packed(inst, "u8", "s32", "r = Saturate<u8>(x - y)", false);
        case Op::PackedSaturatedSubS8: return Packed(inst, "s8", "s32", "r = Saturate<s8>(x - y)", false);
        case Op::PackedSaturatedAddU16: return Packed(inst, "u16", "s32", "r = Saturate<u16>(x + y)", false);
        case Op::PackedSaturatedAddS16: return Packed(inst, "s16", "s32", "r = Saturate<s16>(x + y)", false);
        case Op::PackedSaturatedSubU16: return Packed(inst, "u16", "s32", "r = Saturate<u16>(x - y)", false);
        case Op::PackedSaturatedSubS16: return Packed(inst, "s16", "s32", "r = Saturate<s16>(x - y)", false);

        // Floating point
        case Op::FPAbs32: Define(inst, fmt::format("FPAbs32({})", Arg(inst, 0))); return;
        case Op::FPAbs64: Define(inst, fmt::format("FPAbs64({})", Arg(inst, 0))); return;
        case Op::FPNeg32: Define(inst, fmt::format("FPNeg32({})", Arg(inst, 0))); return;
        case Op::FPNeg64: Define(inst, fmt::format("FPNeg64({})", Arg(inst, 0))); return;
        case Op::FPSqrt32: Define(inst, fmt::format("FPSqrt32({})", Arg(inst, 0))); return;
        case Op::FPSqrt64: Define(inst, fmt::format("FPSqrt64({})", Arg(inst, 0))); return;
        case Op::FPAdd32: return FP2(inst, "FPAdd32");
        case Op::FPAdd64: return FP2(inst, "FPAdd64");
        case Op::FPSub32: return FP2(inst, "FPSub32");
        case Op::FPSub64: return FP2(inst, "FPSub64");
        case Op::FPMul32: return FP2(inst, "FPMul32");
        case Op::FPMul64: return FP2(inst, "FPMul64");
        case Op::FPDiv32: return FP2(inst, "FPDiv32");
        case Op::FPDiv64: return FP2(inst, "FPDiv64");
        case Op::FPMulAdd32:
            Define(inst, fmt::format("FPMulAdd32({}, {}, {})", Arg(inst, 0), Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::FPMulAdd64:
            Define(inst, fmt::format("FPMulAdd64({}, {}, {})", Arg(inst, 0), Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::FPCompare32:
            Define(inst, fmt::format("FPCompare32({}, {}, {})", Arg(inst, 0), Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::FPCompare64:
            Define(inst, fmt::format("FPCompare64({}, {}, {})", Arg(inst, 0), Arg(inst, 1), Arg(inst, 2)));
            return;
        case Op::FPSingleToDouble:
            Define(inst, fmt::format("FPSingleToDouble({})", Arg(inst, 0)));
            return;
        case Op::FPDoubleToSingle:
            if (ImmU64(inst, 1) > 4)
                throw Unsupported{"FPDoubleToSingle ToOdd"};
            Define(inst, fmt::format("FPDoubleToSingle({})", Arg(inst, 0)));
            return;
        case Op::FPSingleToFixedS32: return ToFixed(inst, "FPSingleToFixedS32");
        case Op::FPSingleToFixedU32: return ToFixed(inst, "FPSingleToFixedU32");
        case Op::FPDoubleToFixedS32: return ToFixed(inst, "FPDoubleToFixedS32");
        case Op::FPDoubleToFixedU32: return ToFixed(inst, "FPDoubleToFixedU32");
        case Op::FPFixedS32ToSingle: return FromFixed(inst, "FPFixedS32ToSingle");
        case Op::FPFixedU32ToSingle: return FromFixed(inst, "FPFixedU32ToSingle");
        case Op::FPFixedS32ToDouble: return FromFixed(inst, "FPFixedS32ToDouble");
        case Op::FPFixedU32ToDouble: return FromFixed(inst, "FPFixedU32ToDouble");

        default:
            throw Unsupported{IR::GetNameOf(inst.GetOpcode())};
        }
    }

    static unsigned ExtIndex(A32::ExtReg reg) {
        if (A32::IsSingleExtReg(reg))
            return static_cast<unsigned>(A32::RegNumber(reg));
        if (A32::IsDoubleExtReg(reg))
            return static_cast<unsigned>(A32::RegNumber(reg) * 2);
        throw Unsupported{"quad extension register"};
    }

    static unsigned Bits(IR::Type type) {
        switch (type) {
        case IR::Type::U8:
            return 8;
        case IR::Type::U16:
            return 16;
        case IR::Type::U32:
            return 32;
        case IR::Type::U64:
            return 64;
        default:
            throw Unsupported{"memory width"};
        }
    }

    std::string Coproc(const IR::Inst& inst) {
        const auto info = inst.GetArg(0).GetCoprocInfo();
        u64 packed = 0;
        for (std::size_t i = 0; i < info.size(); ++i)
            packed |= u64(info[i]) << (8 * i);
        return fmt::format("0x{:x}ull", packed);
    }

    static bool IsAddSub32(const IR::Inst& inst) {
        return inst.GetOpcode() == Op::Add32 || inst.GetOpcode() == Op::Sub32;
    }

    /// The operand was constant-folded; Dynarmic still TSTs the value.
    std::string ImmediateNZ(const IR::Inst& inst) {
        const IR::Value value = inst.GetArg(0);
        if (value.GetType() == IR::Type::U64)
            return fmt::format("NZ64({})", Arg(inst, 0));
        if (value.GetType() == IR::Type::U32)
            return fmt::format("NZ({})", Arg(inst, 0));
        throw Unsupported{"NZ of non-integer immediate"};
    }

    std::string NZ(const IR::Inst& parent) {
        // Without host flags from the operation itself, Dynarmic tests the
        // result (TST): N and Z from the value, C and V clear.
        if (IsAddSub32(parent))
            return Name(&parent) + "_f.nzcv & 0xC0000000u";
        if (parent.GetType() == IR::Type::U64)
            return fmt::format("NZ64({})", Name(&parent));
        if (parent.GetType() != IR::Type::U32)
            throw Unsupported{"NZ of non-integer"};
        return fmt::format("NZ({})", Name(&parent));
    }

    std::string Carry(const IR::Inst& parent) {
        const auto arg = [&](std::size_t i) { return Arg(parent, i); };
        switch (parent.GetOpcode()) {
        case Op::Add32:
        case Op::Sub32:
            return fmt::format("({}_f.nzcv >> 29) & 1", Name(&parent));
        case Op::LogicalShiftLeft32:
            return fmt::format("LslCarry({}, {}, {})", arg(0), arg(1), arg(2));
        case Op::LogicalShiftRight32:
            return fmt::format("LsrCarry({}, {}, {})", arg(0), arg(1), arg(2));
        case Op::ArithmeticShiftRight32:
            return fmt::format("AsrCarry({}, {}, {})", arg(0), arg(1), arg(2));
        case Op::RotateRight32:
            return fmt::format("RorCarry({}, {}, {})", arg(0), arg(1), arg(2));
        case Op::RotateRightExtended:
            return fmt::format("({} & 1) != 0", arg(0));
        case Op::MostSignificantWord:
            return fmt::format("(({} >> 31) & 1) != 0", arg(0));
        default:
            throw Unsupported{"carry from " + IR::GetNameOf(parent.GetOpcode())};
        }
    }

    std::string Overflow(const IR::Inst& parent) {
        switch (parent.GetOpcode()) {
        case Op::Add32:
        case Op::Sub32:
            return fmt::format("({}_f.nzcv >> 28) & 1", Name(&parent));
        case Op::UnsignedSaturation:
        case Op::SignedSaturation:
            return Name(&parent) + "_q";
        default:
            throw Unsupported{"overflow from " + IR::GetNameOf(parent.GetOpcode())};
        }
    }

    void Packed(const IR::Inst& inst, const char* lane, const char* wide, const char* body, bool sets_ge) {
        const std::string name = Reserve(inst);
        if (sets_ge) {
            packed_ge.insert(&inst);
            Line(fmt::format("u32 {}_ge;", name));
        }
        // Clang vectorizes the saturating forms into NEON SQADD/UQSUB..., which
        // set the host FPSR.QC that carries the guest's FPSCR. The ARM
        // instructions set no flags, so keep the host FPSR unchanged.
        const bool saturating = std::string_view(body).find("Saturate") != std::string_view::npos;
        if (saturating) {
            Line(fmt::format("[[maybe_unused]] const u32 {} = PackedSaturating<{}, {}>({}, {}, "
                             "[](auto x, auto y, bool& ge) {{ {} r; {}; (void)ge; return r; }});",
                             name, lane, wide, Arg(inst, 0), Arg(inst, 1), wide, body));
            return;
        }
        Line(fmt::format("[[maybe_unused]] const u32 {} = Packed<{}, {}>({}, {}, {}, "
                         "[](auto x, auto y, bool& ge) {{ {} r; {}; (void)ge; return r; }});",
                         name, lane, wide, Arg(inst, 0), Arg(inst, 1),
                         sets_ge ? "&" + name + "_ge" : std::string("nullptr"), wide, body));
    }

    void FP2(const IR::Inst& inst, const char* fn) {
        Define(inst, fmt::format("{}({}, {})", fn, Arg(inst, 0), Arg(inst, 1)));
    }

    void ToFixed(const IR::Inst& inst, const char* fn) {
        const u64 fbits = ImmU64(inst, 1), rounding = ImmU64(inst, 2);
        if (rounding > 4 || (rounding != 3 && fbits != 0))
            throw Unsupported{std::string(fn) + " rounding"};
        Define(inst, fmt::format("{}<{}, {}>({})", fn, fbits, rounding, Arg(inst, 0)));
    }

    void FromFixed(const IR::Inst& inst, const char* fn) {
        const u64 fbits = ImmU64(inst, 1), rounding = ImmU64(inst, 2);
        if (rounding > 3)
            throw Unsupported{std::string(fn) + " rounding"};
        const u32 block_rmode = (A32::LocationDescriptor{block.Location()}.FPSCR().Value() >> 22) & 3;
        Define(inst, fmt::format("{}<{}, {}, {}>({})", fn, fbits, rounding, block_rmode, Arg(inst, 0)));
    }

    // ---- Terminals ------------------------------------------------------------

    void SetUpper(const IR::LocationDescriptor& next, const IR::LocationDescriptor& initial) {
        const u32 new_upper = Upper(next);
        if (Upper(initial) != new_upper)
            Line(fmt::format("s->upper_location_descriptor = 0x{:x}u;", new_upper));
    }

    /// Leaves to the dispatcher at `next` (PC and upper already in state for
    /// dynamic exits).
    void ExitTo(const IR::LocationDescriptor& next) {
        Line(fmt::format("s->regs[15] = 0x{:x}u;", A32::LocationDescriptor{next}.PC()));
        Line("return s->Descriptor();");
    }

    bool CanChain(const IR::LocationDescriptor& next) const {
        // A different FPSCR mode needs the dispatcher to reprogram the host FPCR.
        return translated.contains(next.Value()) &&
               (Upper(next) & 0xFFFF0000) == (Upper(block.Location()) & 0xFFFF0000);
    }

    void Chain(const IR::LocationDescriptor& next) {
        out.callees.insert(next.Value());
        Line(fmt::format("DEKUI_AOT_MUSTTAIL return {}(s);", BlockSymbol(next.Value())));
    }

    void Terminal(const IR::Terminal& terminal) {
        const IR::LocationDescriptor initial =
            A32::LocationDescriptor{block.Location()}.SetSingleStepping(false);
        struct Visitor {
            BlockEmitter& e;
            IR::LocationDescriptor initial;
            void operator()(const IR::Term::Invalid&) { throw Unsupported{"invalid terminal"}; }
            void operator()(const IR::Term::Interpret&) { throw Unsupported{"Interpret terminal"}; }
            void operator()(const IR::Term::ReturnToDispatch&) { e.Line("return s->Descriptor();"); }
            void operator()(const IR::Term::PopRSBHint&) { e.Line("return s->Descriptor();"); }
            void operator()(const IR::Term::FastDispatchHint&) { e.Line("return s->Descriptor();"); }
            void operator()(const IR::Term::LinkBlock& t) {
                e.SetUpper(t.next, initial);
                if (e.CanChain(t.next)) {
                    e.Line("if (s->ticks > 0) {");
                    ++e.indent;
                    e.Chain(t.next);
                    --e.indent;
                    e.Line("}");
                }
                e.ExitTo(t.next);
            }
            void operator()(const IR::Term::LinkBlockFast& t) {
                e.SetUpper(t.next, initial);
                if (e.CanChain(t.next))
                    e.Chain(t.next);
                else
                    e.ExitTo(t.next);
            }
            void operator()(const IR::Term::If& t) {
                e.Line(fmt::format("if (ConditionPassed(s->cpsr_nzcv, {})) {{", static_cast<unsigned>(t.if_)));
                Nested(t.then_);
                e.Line("} else {");
                Nested(t.else_);
                e.Line("}");
            }
            void operator()(const IR::Term::CheckBit& t) {
                e.Line("if (s->check_bit) {");
                Nested(t.then_);
                e.Line("} else {");
                Nested(t.else_);
                e.Line("}");
            }
            void operator()(const IR::Term::CheckHalt& t) {
                e.Line("if (s->halt) return s->Descriptor();");
                boost::apply_visitor(*this, t.else_);
            }
            void Nested(const IR::Terminal& term) {
                ++e.indent;
                boost::apply_visitor(*this, term);
                --e.indent;
            }
        } visitor{*this, initial};
        boost::apply_visitor(visitor, terminal);
    }

    const IR::Block& block;
    const std::set<u64>& translated;
    EmittedBlock& out;
    std::unordered_map<const IR::Inst*, std::string> names;
    std::set<const IR::Inst*> packed_ge;
    int indent = 1;
};

constexpr const char* Preamble = R"(// Generated by dekui_aot from the user's own game dump. Do not distribute.
#include "core/arm/aot/block_support.h"
#define DEKUI_AOT_BLOCK [[gnu::visibility("hidden")]]
using namespace DekuiAot::Rt;
)";

} // namespace

std::string BlockSymbol(std::uint64_t descriptor) {
    return fmt::format("dekui_b_{:08x}_{:08x}", u32(descriptor >> 32), u32(descriptor));
}

bool EmitBlock(const Dynarmic::IR::Block& block, const std::set<std::uint64_t>& translated,
               EmittedBlock& out, std::string& unsupported) {
    try {
        BlockEmitter{block, translated, out}.Emit();
        return true;
    } catch (const Unsupported& error) {
        unsupported = error.what;
        out = {};
        return false;
    }
}

EmitStats EmitProgram(const Image& image, const Program& program,
                      const std::filesystem::path& directory, std::size_t shards) {
    EmitStats stats;

    // Pass 1: which blocks can be expressed at all. Chains may only target those.
    std::set<u64> translated;
    for (const auto& [descriptor, info] : program.blocks) {
        const IR::Block block = TranslateBlock(image, descriptor);
        EmittedBlock scratch;
        std::string unsupported;
        if (EmitBlock(block, {}, scratch, unsupported))
            translated.insert(descriptor);
        else
            ++stats.unsupported[unsupported];
    }

    // Pass 2: write the shards. Blocks are spread in address order so related
    // code tends to share a translation unit.
    std::filesystem::create_directories(directory);
    std::vector<u64> order(translated.begin(), translated.end());
    std::sort(order.begin(), order.end(), [](u64 a, u64 b) {
        return std::pair{u32(a), a >> 32} < std::pair{u32(b), b >> 32};
    });
    const std::size_t per_shard = (order.size() + shards - 1) / std::max<std::size_t>(shards, 1);
    std::vector<std::string> sources;
    for (std::size_t shard = 0; shard * per_shard < order.size(); ++shard) {
        std::string body;
        std::set<u64> defined, callees;
        for (std::size_t i = shard * per_shard; i < std::min(order.size(), (shard + 1) * per_shard); ++i) {
            const IR::Block block = TranslateBlock(image, order[i]);
            EmittedBlock emitted;
            std::string unsupported;
            if (!EmitBlock(block, translated, emitted, unsupported))
                throw std::runtime_error("block became unsupported on second pass");
            body += emitted.code;
            defined.insert(order[i]);
            callees.insert(emitted.callees.begin(), emitted.callees.end());
            ++stats.emitted;
        }
        std::string text = Preamble;
        for (const u64 callee : callees)
            if (!defined.contains(callee))
                text += fmt::format("extern \"C\" DEKUI_AOT_BLOCK u64 {}(State*);\n", BlockSymbol(callee));
        text += "\n";
        // Forward declarations let blocks in one shard chain in any order.
        for (const u64 descriptor : defined)
            text += fmt::format("extern \"C\" DEKUI_AOT_BLOCK u64 {}(State*);\n", BlockSymbol(descriptor));
        text += "\n" + body;
        const std::string name = fmt::format("blocks_{:03}.cpp", shard);
        std::ofstream(directory / name) << text;
        sources.push_back(name);
    }

    // Block table, sorted by descriptor for binary search.
    std::string table = Preamble;
    for (const u64 descriptor : translated)
        table += fmt::format("extern \"C\" DEKUI_AOT_BLOCK u64 {}(State*);\n", BlockSymbol(descriptor));
    table += "\nnamespace {\nconstexpr DekuiAot::Abi::BlockEntry entries[] = {\n";
    for (const u64 descriptor : translated)
        table += fmt::format("    {{0x{:x}ull, {}}},\n", descriptor, BlockSymbol(descriptor));
    table += "};\n} // namespace\n\n";
    table += fmt::format("extern \"C\" const DekuiAot::Abi::Module dekui_aot_module = {{\n"
                         "    DekuiAot::Abi::Version, \"{}\", 0x{:x}u, entries, {}u}};\n",
                         image.Sha256(), image.CodeSize(), translated.size());
    std::ofstream(directory / "block_table.cpp") << table;
    sources.push_back("block_table.cpp");

    std::ofstream list(directory / "sources.cmake");
    list << "set(DEKUI_AOT_GENERATED_SOURCES\n";
    for (const auto& source : sources)
        list << "    \"${CMAKE_CURRENT_LIST_DIR}/" << source << "\"\n";
    list << ")\n";
    return stats;
}

} // namespace DekuiAot
