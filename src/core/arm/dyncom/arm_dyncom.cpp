// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <memory>
#include "common/logging/log.h"
#include "common/hitch_trace.h"
#include "core/arm/aot/runtime.h"
#include "core/arm/dyncom/arm_dyncom.h"
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/svc.h"
#ifdef ENABLE_MM3D_PRESENTATION
#include "core/arm/mm3d_guest_hooks.h"
#endif

namespace Core {

#ifdef ENABLE_MM3D_PRESENTATION
// Adapts the interpreter's state for the backend-neutral MM3D hooks.
struct ARM_DynCom::MM3DHookState {
    class Cpu final : public MM3DGuestHooks::Cpu {
    public:
        Cpu(ARMul_State& state, u32 id) : state(state), id(id) {}
        std::array<u32, 16>& Regs() override { return state.Reg; }
        std::array<u32, 64>& ExtRegs() override { return state.ExtReg; }
        u32 Cpsr() const override { return state.Cpsr; }
        void SetCpsr(u32 value) override { state.Cpsr = value; }
        // Camera-write tracing steps the Dynarmic JIT and is unavailable here.
        bool CameraTracing() const override { return false; }
        u32 CoreId() const override { return id; }

    private:
        ARMul_State& state;
        u32 id;
    };

    MM3DHookState(Core::System& system, Memory::MemorySystem& memory, ARMul_State& state, u32 id)
        : cpu(state, id), hooks(system, memory, cpu) {}

    Cpu cpu;
    MM3DGuestHooks hooks;
};
#endif

// Lets AOT-translated code run on this interpreter's state (see aot/runtime.h).
struct ARM_DynCom::AotState final : Aot::Host {
    AotState(Core::System& system, Memory::MemorySystem& memory, ARMul_State& cpu,
             ARM_DynCom& core)
        : system(system), cpu(cpu), core(core), runner(memory, *this) {}

    void Load(DekuiAot::Abi::State& s) override {
        std::copy(cpu.Reg.begin(), cpu.Reg.end(), s.regs);
        s.SetCpsr(cpu.Cpsr);
        std::copy(cpu.ExtReg.begin(), cpu.ExtReg.end(), s.ext_regs);
        s.SetFpscr(cpu.VFP[VFP_FPSCR]);
        s.exclusive_state = cpu.HasExclusiveAccess() ? 1 : 0;
    }

    void Store(const DekuiAot::Abi::State& s) override {
        std::copy(std::begin(s.regs), std::end(s.regs), cpu.Reg.begin());
        cpu.Cpsr = s.Cpsr();
        cpu.TFlag = (cpu.Cpsr >> 5) & 1;
        std::copy(std::begin(s.ext_regs), std::end(s.ext_regs), cpu.ExtReg.begin());
        cpu.VFP[VFP_FPSCR] = s.Fpscr();
        if (!s.exclusive_state && cpu.HasExclusiveAccess())
            cpu.UnsetExclusiveMemoryAddress();
    }

    // As SWI_INST, except R15 already holds the next instruction (Dynarmic's
    // convention, which the MM3D hooks expect anyway).
    void CallSVC(u32 swi) override {
#ifdef ENABLE_MM3D_PRESENTATION
        const auto hook_start = std::chrono::steady_clock::now();
        if (cpu.mm3d_hooks && cpu.mm3d_hooks->CallSVC(swi)) {
            Common::HitchTrace::AddCall("hook", nullptr, swi,
                                        std::chrono::steady_clock::now() - hook_start);
            return;
        }
#endif
        Kernel::SVCContext{system}.CallSVC(swi & 0xFFFF);
        // The kernel would ERET here, which clears the local monitor.
        cpu.UnsetExclusiveMemoryAddress();
    }

    void ExceptionRaised(u32 pc, u32 exception) override {
        // Dynarmic::A32::Exception: 0-2 undefined/unpredictable/decode, 12 NX fault, 8 breakpoint.
        // The rest are hints Citra ignores.
        if (exception <= 2 || exception == 8 || exception == 12)
            LOG_CRITICAL(Core_ARM11, "AOT: guest exception {} at {:08x}", exception, pc);
    }

    bool HaltRequested() override {
        return cpu.NumInstrsToExecute == 0;
    }
    u64 TicksRemaining() override {
        return cpu.NumInstrsToExecute;
    }
    void AddTicks(u64 ticks) override {
        // Ask the core each time: loading a save state replaces its timer.
        core.GetTimer().AddTicks(ticks);
        cpu.NumInstrsToExecute = ticks >= cpu.NumInstrsToExecute ? 0 : cpu.NumInstrsToExecute - ticks;
    }

    u64 ExclusiveRead(u32 vaddr, unsigned bytes) override {
        cpu.SetExclusiveMemoryAddress(vaddr);
        switch (bytes) {
        case 1:
            return cpu.memory.Read8(vaddr);
        case 2:
            return cpu.memory.Read16(vaddr);
        case 4:
            return cpu.memory.Read32(vaddr);
        default:
            return cpu.memory.Read64(vaddr);
        }
    }

    bool ExclusiveWrite(u32 vaddr, u64 value, unsigned bytes) override {
        if (!cpu.IsExclusiveMemoryAccess(vaddr))
            return false;
        cpu.UnsetExclusiveMemoryAddress();
        switch (bytes) {
        case 1:
            cpu.memory.Write8(vaddr, static_cast<u8>(value));
            break;
        case 2:
            cpu.memory.Write16(vaddr, static_cast<u16>(value));
            break;
        case 4:
            cpu.memory.Write32(vaddr, static_cast<u32>(value));
            break;
        default:
            cpu.memory.Write64(vaddr, value);
            break;
        }
        return true;
    }

    // The registers DynarmicCP15 exposes, mapped onto DynCom's CP15 array.
    u32* Cp15Register(bool get, bool two, unsigned opc1, unsigned crn, unsigned crm,
                      unsigned opc2) override {
        if (two || opc1 != 0)
            return nullptr;
        if (crn == 13 && crm == 0 && opc2 == 2)
            return &cpu.CP15[CP15_THREAD_UPRW];
        if (crn == 13 && crm == 0 && opc2 == 3)
            return get ? &cpu.CP15[CP15_THREAD_URO] : nullptr;
        if (!get && crn == 7 && ((crm == 5 && opc2 == 4) || (crm == 10 && (opc2 == 4 || opc2 == 5))))
            return &ignored_write;
        return nullptr;
    }

    Core::System& system;
    ARMul_State& cpu;
    ARM_DynCom& core;
    Aot::Runner runner;
    u32 ignored_write = 0;
};

ARM_DynCom::ARM_DynCom(Core::System& system_, Memory::MemorySystem& memory,
                       PrivilegeMode initial_mode, u32 id,
                       std::shared_ptr<Core::Timing::Timer> timer)
    : ARM_Interface(id, timer), system(system_) {
    state = std::make_unique<ARMul_State>(system, memory, initial_mode);
#ifdef ENABLE_MM3D_PRESENTATION
    mm3d = std::make_unique<MM3DHookState>(system, memory, *state, id);
    state->mm3d_hooks = &mm3d->hooks;
#endif
    if (Aot::LinkedModule() && timer) {
        aot = std::make_unique<AotState>(system, memory, *state, *this);
        state->aot_runner = &aot->runner;
    }
}

ARM_DynCom::~ARM_DynCom() {}

void ARM_DynCom::Run() {
    if (break_flag) [[unlikely]] {
        return;
    }
    const Common::HitchTrace::Scope hitch_scope{Common::HitchTrace::Phase::Cpu};
#ifdef ENABLE_MM3D_PRESENTATION
    const auto page_table = state->memory.GetCurrentPageTable();
    mm3d->hooks.BeforeRun(page_table.get());
#endif
    ExecuteInstructions(std::max<s64>(timer->GetDowncount(), 0));
#ifdef ENABLE_MM3D_PRESENTATION
    mm3d->hooks.AfterRun(page_table.get());
#endif
}

void ARM_DynCom::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }
    ExecuteInstructions(1);
}

void ARM_DynCom::ClearInstructionCache() {
#ifdef ENABLE_MM3D_PRESENTATION
    mm3d->hooks.ResetMenuState();
#endif
    ClearTranslations();
}

void ARM_DynCom::ClearTranslations() {
    state->instruction_cache.clear();
    trans_cache_buf_top = 0;
}

void ARM_DynCom::InvalidateCacheRange(u32, std::size_t) {
    ClearTranslations();
}

void ARM_DynCom::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    ClearTranslations();
}

std::shared_ptr<Memory::PageTable> ARM_DynCom::GetPageTable() const {
    return nullptr;
}

void ARM_DynCom::SetPC(u32 pc) {
    state->Reg[15] = pc;
}

u32 ARM_DynCom::GetPC() const {
    return state->Reg[15];
}

u32 ARM_DynCom::GetReg(int index) const {
    return state->Reg[index];
}

void ARM_DynCom::SetReg(int index, u32 value) {
    state->Reg[index] = value;
}

u32 ARM_DynCom::GetVFPReg(int index) const {
    return state->ExtReg[index];
}

void ARM_DynCom::SetVFPReg(int index, u32 value) {
    state->ExtReg[index] = value;
}

u32 ARM_DynCom::GetVFPSystemReg(VFPSystemRegister reg) const {
    return state->VFP[reg];
}

void ARM_DynCom::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    state->VFP[reg] = value;
}

u32 ARM_DynCom::GetCPSR() const {
    return state->Cpsr;
}

void ARM_DynCom::SetCPSR(u32 cpsr) {
    state->Cpsr = cpsr;
}

u32 ARM_DynCom::GetCP15Register(CP15Register reg) const {
    return state->CP15[reg];
}

void ARM_DynCom::SetCP15Register(CP15Register reg, u32 value) {
    state->CP15[reg] = value;
}

void ARM_DynCom::ExecuteInstructions(u64 num_instructions) {
    state->NumInstrsToExecute = num_instructions;
    const u32 ticks_executed = InterpreterMainLoop(state.get());
    if (timer) {
        timer->AddTicks(ticks_executed);
    }
    state->ServeBreak();
}

void ARM_DynCom::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = state->Reg;
    ctx.cpsr = state->Cpsr;
    ctx.fpu_registers = state->ExtReg;
    ctx.fpscr = state->VFP[VFP_FPSCR];
    ctx.fpexc = state->VFP[VFP_FPEXC];
}

void ARM_DynCom::LoadContext(const ThreadContext& ctx) {
    state->Reg = ctx.cpu_registers;
    state->Cpsr = ctx.cpsr;
    state->ExtReg = ctx.fpu_registers;
    state->VFP[VFP_FPSCR] = ctx.fpscr;
    state->VFP[VFP_FPEXC] = ctx.fpexc;
}

void ARM_DynCom::PrepareReschedule() {
    state->NumInstrsToExecute = 0;
}

} // namespace Core
