// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef ENABLE_MM3D_PRESENTATION
#include "core/arm/mm3d_guest_hooks.h"
#endif
#include <csignal>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include "core/arm/aot_coverage.h"
#include <cstring>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/optimization_flags.h>
#include "common/assert.h"
#include "common/hitch_trace.h"
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "core/arm/exception_handler.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

#ifndef SIGILL
constexpr u32 SIGILL = 4;
#endif

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

namespace Core {

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

#ifdef ENABLE_MM3D_PRESENTATION
    // Adapts this JIT's state for the backend-neutral MM3D hooks.
    class HookCpu final : public MM3DGuestHooks::Cpu {
    public:
        explicit HookCpu(ARM_Dynarmic& parent) : parent(parent) {}
        std::array<u32, 16>& Regs() override { return parent.jit->Regs(); }
        std::array<u32, 64>& ExtRegs() override { return parent.jit->ExtRegs(); }
        u32 Cpsr() const override { return parent.jit->Cpsr(); }
        void SetCpsr(u32 value) override { parent.jit->SetCpsr(value); }
        bool CameraTracing() const override { return parent.camera_tracing; }
        u32 CoreId() const override { return parent.GetID(); }

    private:
        ARM_Dynarmic& parent;
    };
#endif
    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
#ifdef ENABLE_MM3D_PRESENTATION
        return hooks.ReadCode(vaddr);
#else
        return memory.Read32OrNullopt(vaddr);
#endif
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
#ifdef ENABLE_MM3D_PRESENTATION
        return hooks.Read32(vaddr);
#else
        return memory.Read32(vaddr);
#endif
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override { return memory.Read64(vaddr); }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
        memory.Write32(vaddr, value);
#ifdef ENABLE_MM3D_PRESENTATION
        hooks.TraceCameraWrite(vaddr, value);
#endif
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
        memory.Write64(vaddr, value);
#ifdef ENABLE_MM3D_PRESENTATION
        hooks.TraceCameraWrite(vaddr, std::uint32_t(value));
        hooks.TraceCameraWrite(vaddr + 4, std::uint32_t(value >> 32));
#endif
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
        return memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
        return memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
        const bool written = memory.WriteExclusive32(vaddr, value, expected);
#ifdef ENABLE_MM3D_PRESENTATION
        if(written) {
            hooks.TraceCameraWrite(vaddr, std::uint32_t(value));
        }
#endif
        return written;
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
        const bool written = memory.WriteExclusive64(vaddr, value, expected);
#ifdef ENABLE_MM3D_PRESENTATION
        if(written) {
            hooks.TraceCameraWrite(vaddr, std::uint32_t(value));
            hooks.TraceCameraWrite(vaddr + 4, std::uint32_t(value >> 32));
        }
#endif
        return written;
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}",
                        pc, MemoryReadCode(pc).value(), num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
#ifdef ENABLE_MM3D_PRESENTATION
        if (hooks.CallSVC(swi))
            return;
#endif
        svc_context.CallSVC(swi);
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
#ifdef ENABLE_GDBSTUB
            if (GDBStub::IsConnected()) {
                parent.SetPC(pc);
                parent.ServeBreak(SIGTRAP);
                return;
            }
#endif
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        }

        static constexpr auto ExceptionToString = [](Dynarmic::A32::Exception e) -> std::string {
            switch (e) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
                return "UndefinedInstruction";
            case Dynarmic::A32::Exception::UnpredictableInstruction:
                return "UnpredictableInstruction";
            case Dynarmic::A32::Exception::DecodeError:
                return "DecodeError";
            case Dynarmic::A32::Exception::NoExecuteFault:
                return "NoExecuteFault";
            case Dynarmic::A32::Exception::Breakpoint:
                return "Breakpoint";
            default:
                return fmt::format("Unknown({})", e);
            }
        };

        parent.SetPC(pc);
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            parent.ServeBreak(SIGILL);
        } else
#endif
        {
            Core::ExceptionType exc_type;
            switch (exception) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
            case Dynarmic::A32::Exception::UnpredictableInstruction:
            case Dynarmic::A32::Exception::DecodeError:
            case Dynarmic::A32::Exception::NoExecuteFault:
                exc_type = Core::ExceptionType::UndefinedInstruction;
                break;
            case Dynarmic::A32::Exception::Breakpoint:
                exc_type = Core::ExceptionType::Break;
                break;
            default:
                exc_type = Core::ExceptionType::UndefinedInstruction;
                break;
            }
            Core::LogException(parent.system, exc_type);
        }
    }

    void AddTicks(std::uint64_t ticks) override {
        parent.GetTimer().AddTicks(ticks);
    }
    std::uint64_t GetTicksRemaining() override {
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }
    void PreCodeTranslationHook(bool is_thumb, VAddr pc, Dynarmic::A32::IREmitter& ir) override {
        if (AotCoverage::Enabled()) {
            AotCoverage::RecordInstruction(is_thumb, pc, ir.block.Location().Value());
        }
    }

    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
#ifdef ENABLE_MM3D_PRESENTATION
    // Declared after `parent` so it is initialized after it.
    HookCpu hook_cpu{parent};
    MM3DGuestHooks hooks{parent.system, parent.memory, hook_cpu};
#endif
};

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
    SetPageTable(memory.GetCurrentPageTable());
}

ARM_Dynarmic::~ARM_Dynarmic() = default;

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    const Common::HitchTrace::Scope hitch_scope{Common::HitchTrace::Phase::Cpu};
    if (break_flag) [[unlikely]] {
        return;
    }
#ifdef CITRA_VISIONOS
    retired_jits.clear();
#endif

#ifdef ENABLE_MM3D_PRESENTATION
    cb->hooks.BeforeRun(current_page_table.get());
    UpdateCameraTraceMode();
    if(camera_tracing && MM3D::CameraWriter::ShouldTrace()) {
        // Bound host time independently of guest progress/rendering. Each Step
        // returns after one instruction, including conditional and idle-loop code.
        for(unsigned i=0; i<256 && GetTimer().GetDowncount()>0; ++i) {
            if(!MM3D::CameraWriter::ShouldTrace()) break;
            const auto halt = MM3D::CameraWriter::StepWithSnapshot(*jit, cb->hooks.camera_instruction);
            if((halt & Dynarmic::HaltReason::UserDefined1) == Dynarmic::HaltReason::UserDefined1) break;
        }
    } else
#endif
    {
#ifdef CITRA_VISIONOS
        running = true;
#endif
        jit->Run();
#ifdef CITRA_VISIONOS
        running = false;
#endif
    }
#ifdef ENABLE_MM3D_PRESENTATION
    cb->hooks.AfterRun(current_page_table.get());
#endif
}

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }

#ifdef ENABLE_MM3D_PRESENTATION
    UpdateCameraTraceMode();
    if(camera_tracing && MM3D::CameraWriter::ShouldTrace()) MM3D::CameraWriter::StepWithSnapshot(*jit, cb->hooks.camera_instruction);
    else
#endif
    {
#ifdef CITRA_VISIONOS
        running = true;
#endif
        jit->Step();
#ifdef CITRA_VISIONOS
        running = false;
#endif
    }
}

#ifdef ENABLE_MM3D_PRESENTATION
void ARM_Dynarmic::UpdateCameraTraceMode() {
    const bool wanted = MM3D::CameraWriter::ShouldTrace();
    if(wanted == camera_tracing) return;
    ThreadContext saved{};
    SaveContext(saved);
    // Switching backends may spuriously fail an outstanding exclusive store;
    // ARM permits this. Do not carry a stale local reservation into the new JIT.
    jit->ClearExclusiveState();
    jit = nullptr;
    jits.swap(camera_other_jits);
    camera_tracing = wanted;
    SetPageTable(current_page_table);
    jit->ClearExclusiveState();
    LoadContext(saved);
}
#endif
void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return jit->Fpscr();
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }

    return UINT_MAX;
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        jit->SetFpscr(value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->SetCpsr(cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }

    return 0;
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }
}

void ARM_Dynarmic::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = jit->Regs();
    ctx.cpsr = jit->Cpsr();
    ctx.fpu_registers = jit->ExtRegs();
    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const ThreadContext& ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->SetCpsr(ctx.cpsr);
    jit->ExtRegs() = ctx.fpu_registers;
    jit->SetFpscr(ctx.fpscr);
    fpexc = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
#ifdef ENABLE_MM3D_PRESENTATION
    cb->hooks.ResetMenuState();
    for(const auto& j : camera_other_jits) j.second->ClearCache();
#endif
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
    jit->InvalidateCacheRange(start_address, length);
#ifdef ENABLE_MM3D_PRESENTATION
    // Also invalidate the dormant mode's cache for the active process.
    auto other = camera_other_jits.find(current_page_table);
    if(other != camera_other_jits.end()) other->second->InvalidateCacheRange(start_address, length);
#endif
}

void ARM_Dynarmic::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    ThreadContext ctx{};
    if (jit) {
        SaveContext(ctx);
    }

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
#ifdef CITRA_VISIONOS
        RetireOtherJits();
#endif
        return;
    }

#ifdef CITRA_VISIONOS
    // Release the old JIT's code cache before allocating the new one.
    RetireOtherJits();
#endif
    auto new_jit = MakeJit();
    jit = new_jit.get();
    LoadContext(ctx);
    jits.emplace(current_page_table, std::move(new_jit));
}

#ifdef CITRA_VISIONOS
void ARM_Dynarmic::RetireOtherJits() {
    for (auto it = jits.begin(); it != jits.end();) {
        if (it->first == current_page_table) {
            ++it;
            continue;
        }
        if (running) {
            retired_jits.push_back(std::move(it->second));
        }
        it = jits.erase(it);
    }
}
#endif

void ARM_Dynarmic::ServeBreak([[maybe_unused]] int signal) {
#ifdef ENABLE_GDBSTUB
    GDBStub::Break(signal);
#endif
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    if (current_page_table
#ifdef ENABLE_MM3D_PRESENTATION
        && !camera_tracing
#endif
    ) {
        config.page_table = &current_page_table->GetPointerArray();
    }
#ifdef CITRA_VISIONOS
    // Every JIT instance draws from the fixed, debugger-prepared code arena.
    config.code_cache_size = 16 * 1024 * 1024;
#endif
#ifdef ENABLE_MM3D_PRESENTATION
    if(camera_tracing) config.code_cache_size = 16 * 1024 * 1024;
#endif
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core
