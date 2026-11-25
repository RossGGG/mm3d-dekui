// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// dekui_aot_check: differential test of AOT-generated blocks against Dynarmic.
//
//   dekui_aot_check <work> [--runs N] [--seed S] [--first DESCRIPTOR] [--limit N] [--verbose]
//
// Every block in the linked module runs from the same pseudo-random guest
// state in Dynarmic (configured as Citra configures it) and in the generated
// code. Registers, flags, FP state, all touched memory, SVC/exception events
// and consumed cycles must match. Needs no game session: memory outside the
// image is filled deterministically on first touch.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/ir/basic_block.h>
#include <fmt/format.h>
#include "core/arm/aot/abi.h"
#include "core/arm/aot/block_support.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "dekui_aot/frontend.h"
#include "dekui_aot/image.h"

extern "C" const DekuiAot::Abi::Module dekui_aot_module;

namespace {

using namespace DekuiAot;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Abi::State;

u64 SplitMix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// Sparse guest memory. Pages materialise on first touch with the image's
/// original bytes or with deterministic noise, so two instances built from
/// the same seed always agree on content nobody wrote.
class GuestMemory {
public:
    GuestMemory(const Image& image, u64 seed) : image(image), seed(seed) {
        page_table.assign(1u << 20, nullptr);
    }

    u8* Page(u32 index) {
        auto [it, inserted] = pages.try_emplace(index);
        if (inserted)
            Fill(index, it->second);
        return it->second.data();
    }

    template <typename T>
    T Read(u32 vaddr) {
        T value{};
        u8* bytes = reinterpret_cast<u8*>(&value);
        for (unsigned i = 0; i < sizeof(T); ++i)
            bytes[i] = Page((vaddr + i) >> 12)[(vaddr + i) & 0xFFF];
        Map(vaddr);
        return value;
    }

    template <typename T>
    void Write(u32 vaddr, T value) {
        const u8* bytes = reinterpret_cast<const u8*>(&value);
        for (unsigned i = 0; i < sizeof(T); ++i)
            Page((vaddr + i) >> 12)[(vaddr + i) & 0xFFF] = bytes[i];
        Map(vaddr);
    }

    /// Exposes the page to the AOT fast path after its first slow access.
    void Map(u32 vaddr) { page_table[vaddr >> 12] = Page(vaddr >> 12); }

    std::map<u32, std::array<u8, 4096>> pages;
    std::vector<u8*> page_table;

private:
    void Fill(u32 index, std::array<u8, 4096>& page) const {
        for (u32 offset = 0; offset < 4096; offset += 4) {
            const u32 vaddr = (index << 12) + offset;
            u32 word;
            if (const auto original = image.Read32(vaddr))
                word = *original;
            else
                word = static_cast<u32>(SplitMix(seed ^ vaddr));
            std::memcpy(page.data() + offset, &word, 4);
        }
    }

    const Image& image;
    u64 seed;
};

struct Event {
    std::string what;
    std::array<u32, 16> regs;
    bool operator==(const Event&) const = default;
};

/// CP15 with the registers Citra's DynarmicCP15 exposes.
struct Cp15State {
    u32 uprw = 0, uro = 0, flush = 0, dsb = 0, dmb = 0;
};

using Dynarmic::A32::CoprocReg;

std::optional<u32*> Cp15Send(Cp15State& s, bool two, unsigned opc1, CoprocReg crn, CoprocReg crm, unsigned opc2) {
    if (two || opc1 != 0)
        return std::nullopt;
    if (crn == CoprocReg::C7 && crm == CoprocReg::C5 && opc2 == 4)
        return &s.flush;
    if (crn == CoprocReg::C7 && crm == CoprocReg::C10 && opc2 == 4)
        return &s.dsb;
    if (crn == CoprocReg::C7 && crm == CoprocReg::C10 && opc2 == 5)
        return &s.dmb;
    if (crn == CoprocReg::C13 && crm == CoprocReg::C0 && opc2 == 2)
        return &s.uprw;
    return std::nullopt;
}

std::optional<u32*> Cp15Get(Cp15State& s, bool two, unsigned opc1, CoprocReg crn, CoprocReg crm, unsigned opc2) {
    if (two || opc1 != 0 || crn != CoprocReg::C13 || crm != CoprocReg::C0)
        return std::nullopt;
    if (opc2 == 2)
        return &s.uprw;
    if (opc2 == 3)
        return &s.uro;
    return std::nullopt;
}

/// Set when a run touches something Citra could not run either.
bool* g_bad_run = nullptr;

class DynarmicCp15 final : public Dynarmic::A32::Coprocessor {
public:
    /// With `real` false every access is junk (coprocessors other than CP15).
    DynarmicCp15(Cp15State& state, bool real) : state(state), real(real) {}
    // Citra's CP15 returns "exception" here, which makes Dynarmic assert while
    // compiling. Only junk decodes to these, so flag the run instead.
    static std::uint64_t Bad(void*, std::uint32_t, std::uint32_t) {
        if (g_bad_run)
            *g_bad_run = true;
        return 0;
    }
    static constexpr Callback bad{&Bad, std::nullopt};
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return bad;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg crn, CoprocReg crm, unsigned opc2) override {
        if (auto p = Cp15Send(state, two, opc1, crn, crm, opc2); p && real)
            return *p;
        return bad;
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return bad; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg crn, CoprocReg crm, unsigned opc2) override {
        if (auto p = Cp15Get(state, two, opc1, crn, crm, opc2); p && real)
            return *p;
        return bad;
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return bad; }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<u8>) override { return bad; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<u8>) override { return bad; }

private:
    Cp15State& state;
    bool real;
};

/// Everything a run produces that must agree between the two engines.
struct Outcome {
    std::array<u32, 16> regs{};
    u32 cpsr = 0, fpscr = 0;
    std::array<u32, 64> ext{};
    std::vector<Event> events;
    u64 ticks = 0;
    bool stopped_untranslated = false;
    bool timed_out = false; ///< Dynarmic never left the guest code (endless guest loop).
    bool bad = false; ///< The engine hit something it cannot run (code outside the image).
};

struct InitialState {
    std::array<u32, 16> regs;
    u32 cpsr, fpscr;
    std::array<u32, 64> ext;
    Cp15State cp15;
};

// ---------------------------------------------------------------------------
// Dynarmic side

class JitCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    JitCallbacks(const Image& image) : image(image) {}

    std::optional<u32> MemoryReadCode(u32 vaddr) override {
        auto word = image.ReadCode(vaddr);
        if (!word)
            outcome->bad = true;
        return word;
    }
    // Same block-length cap as dekui_aot's frontend: splitting is invisible to
    // the guest, and unbounded zero-fill blocks make Dynarmic quadratic.
    bool PreCodeReadHook(bool, u32, Dynarmic::A32::IREmitter& ir) override {
        // Dynarmic reuses one IR::Block object, so detect a new translation by
        // its first instruction.
        if (ir.current_location.PC() == Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC())
            block_instructions = 0;
        if (++block_instructions > 512) {
            ir.SetTerm(Dynarmic::IR::Term::LinkBlock{ir.current_location});
            return false;
        }
        return true;
    }
    std::size_t block_instructions = 0;

    u8 MemoryRead8(u32 a) override { return memory->Read<u8>(a); }
    u16 MemoryRead16(u32 a) override { return memory->Read<u16>(a); }
    u32 MemoryRead32(u32 a) override { return memory->Read<u32>(a); }
    u64 MemoryRead64(u32 a) override { return memory->Read<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { memory->Write(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { memory->Write(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { memory->Write(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { memory->Write(a, v); }
    template <typename T>
    bool WriteExclusive(u32 a, T v, T expected) {
        if (memory->Read<T>(a) != expected)
            return false;
        memory->Write(a, v);
        return true;
    }
    bool MemoryWriteExclusive8(u32 a, u8 v, u8 e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive16(u32 a, u16 v, u16 e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive32(u32 a, u32 v, u32 e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive64(u32 a, u64 v, u64 e) override { return WriteExclusive(a, v, e); }

    void InterpreterFallback(u32 pc, size_t) override {
        outcome->events.push_back({fmt::format("interpreter {:08x}", pc), jit->Regs()});
        jit->HaltExecution();
    }
    void CallSVC(u32 swi) override {
        outcome->events.push_back({fmt::format("svc {:x}", swi), jit->Regs()});
        jit->HaltExecution();
    }
    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) override {
        outcome->events.push_back({fmt::format("exception {:08x} {}", pc, static_cast<int>(exception)), jit->Regs()});
        jit->HaltExecution();
    }
    void AddTicks(u64 ticks) override { consumed += ticks; }
    u64 GetTicksRemaining() override { return consumed >= budget ? 0 : budget - consumed; }
    u64 GetTicksForCode(bool is_thumb, u32, u32 instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    const Image& image;
    GuestMemory* memory = nullptr;
    Outcome* outcome = nullptr;
    Dynarmic::A32::Jit* jit = nullptr;
    u64 budget = 1, consumed = 0;
};

// ---------------------------------------------------------------------------
// AOT side

struct AotContext {
    State state;
    GuestMemory* memory = nullptr;
    Outcome* outcome = nullptr;
    Cp15State* cp15 = nullptr;
    Dynarmic::ExclusiveMonitor* monitor = nullptr;
    u64 budget = 1, consumed = 0, cycles_to_run = 0;

    static AotContext& From(State* s) { return *reinterpret_cast<AotContext*>(s); }
    u64 Remaining() const { return consumed >= budget ? 0 : budget - consumed; }
    // Mirrors the arm64 backend's accounting around callbacks.
    void SyncTicksBefore() { consumed += cycles_to_run - static_cast<u64>(state.ticks); }
    void SyncTicksAfter() { cycles_to_run = Remaining(); state.ticks = static_cast<std::int64_t>(cycles_to_run); }
};
static_assert(offsetof(AotContext, state) == 0);

template <typename T>
T AotRead(State* s, u32 a) { return AotContext::From(s).memory->Read<T>(a); }
template <typename T>
void AotWrite(State* s, u32 a, T v) { AotContext::From(s).memory->Write(a, v); }
template <typename T>
T AotExclusiveRead(State* s, u32 a) {
    auto& c = AotContext::From(s);
    return c.monitor->ReadAndMark<T>(0, a, [&] { return c.memory->Read<T>(a); });
}
template <typename T>
u32 AotExclusiveWrite(State* s, u32 a, T v) {
    auto& c = AotContext::From(s);
    return c.monitor->DoExclusiveOperation<T>(0, a, [&](T expected) {
        if (c.memory->Read<T>(a) != expected)
            return false;
        c.memory->Write(a, v);
        return true;
    }) ? 0 : 1;
}

std::array<u32, 16> Regs(const State* s) {
    std::array<u32, 16> regs;
    std::copy(std::begin(s->regs), std::end(s->regs), regs.begin());
    return regs;
}

void AotSvc(State* s, u32 swi) {
    auto& c = AotContext::From(s);
    c.SyncTicksBefore();
    c.outcome->events.push_back({fmt::format("svc {:x}", swi), Regs(s)});
    s->halt = 1;
    c.SyncTicksAfter();
}
void AotException(State* s, u32 pc, u32 exception) {
    auto& c = AotContext::From(s);
    c.SyncTicksBefore();
    c.outcome->events.push_back({fmt::format("exception {:08x} {}", pc, exception), Regs(s)});
    s->halt = 1;
    c.SyncTicksAfter();
}

struct CoprocArgs {
    bool two;
    unsigned opc1;
    CoprocReg crn, crm;
    unsigned opc2;
};
CoprocArgs Decode(u64 info) {
    const auto b = [&](int i) { return static_cast<u8>(info >> (8 * i)); };
    // Layout of Dynarmic's CoprocInfo for one-word transfers:
    // [0]=coproc [1]=two [2]=opc1 [3]=CRn [4]=CRm [5]=opc2
    return {b(1) != 0, b(2), static_cast<CoprocReg>(b(3)), static_cast<CoprocReg>(b(4)), b(5)};
}
void AotCoprocSendOne(State* s, u64 info, u32 value) {
    const auto a = Decode(info);
    auto p = Cp15Send(*AotContext::From(s).cp15, a.two, a.opc1, a.crn, a.crm, a.opc2);
    if (static_cast<u8>(info) != 15 || !p)
        AotContext::From(s).outcome->bad = true;
    else
        **p = value;
}
u32 AotCoprocGetOne(State* s, u64 info) {
    const auto a = Decode(info);
    auto p = Cp15Get(*AotContext::From(s).cp15, a.two, a.opc1, a.crn, a.crm, a.opc2);
    if (static_cast<u8>(info) != 15 || !p) {
        AotContext::From(s).outcome->bad = true;
        return 0;
    }
    return **p;
}
void AotCoprocUnsupported(State* s, u64) { AotContext::From(s).outcome->bad = true; }
void AotCoprocUnsupported2(State* s, u64, u32, u32) { AotContext::From(s).outcome->bad = true; }
u64 AotCoprocUnsupported3(State* s, u64) {
    AotContext::From(s).outcome->bad = true;
    return 0;
}
void AotCoprocUnsupported4(State* s, u64, u32) { AotContext::From(s).outcome->bad = true; }

const Abi::BlockEntry* Lookup(u64 descriptor) {
    const auto* begin = dekui_aot_module.blocks;
    const auto* end = begin + dekui_aot_module.block_count;
    const auto* it = std::lower_bound(begin, end, descriptor,
                                      [](const Abi::BlockEntry& e, u64 d) { return e.descriptor < d; });
    return it != end && it->descriptor == descriptor ? it : nullptr;
}

// ---------------------------------------------------------------------------

class Rng {
public:
    explicit Rng(u64 seed) : state(seed) {}
    u64 Next() { return state = SplitMix(state); }
    u32 Below(u32 n) { return static_cast<u32>(Next() % n); }
    bool Chance(unsigned percent) { return Below(100) < percent; }

private:
    u64 state;
};

u32 RandomWord(Rng& rng, const Image& image) {
    switch (rng.Below(8)) {
    case 0:
        return rng.Below(256);
    case 1:
        return rng.Chance(50) ? 0 : 0xFFFFFFFF;
    case 2: {
        const auto& segments = image.Segments();
        const auto& segment = segments[rng.Below(static_cast<u32>(segments.size()))];
        return segment.address + (rng.Below(segment.size) & ~3u);
    }
    case 3:
        return 0x08000000 + (rng.Below(0x100000) & ~3u); // heap-like
    case 4:
        return 0x10000000 - (rng.Below(0x4000) & ~3u);   // stack-like
    case 5:
        return 0x80000000u ^ rng.Below(16);
    default:
        return static_cast<u32>(rng.Next());
    }
}

u32 RandomFloatBits(Rng& rng) {
    static constexpr u32 specials[] = {0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000,
                                       0xFF800000, 0x7FC00000, 0x7F800001, 0x00000001, 0x807FFFFF,
                                       0x4B000000, 0x4F000000, 0xCF000000, 0x3F000000};
    if (rng.Chance(25))
        return specials[rng.Below(std::size(specials))];
    if (rng.Chance(50)) {
        // A "normal" float in a game-ish range.
        const float f = (static_cast<float>(rng.Below(2000000)) - 1000000.0f) / static_cast<float>(1 + rng.Below(1000));
        u32 bits;
        std::memcpy(&bits, &f, 4);
        return bits;
    }
    return static_cast<u32>(rng.Next());
}

InitialState MakeState(Rng& rng, const Image& image, u64 descriptor) {
    const Dynarmic::A32::LocationDescriptor location{Dynarmic::IR::LocationDescriptor{descriptor}};
    InitialState init;
    for (auto& r : init.regs)
        r = RandomWord(rng, image);
    init.regs[15] = location.PC();
    for (auto& e : init.ext)
        e = RandomFloatBits(rng);
    u32 cpsr = static_cast<u32>(rng.Next()) & 0xF80F0000; // NZCVQ + GE
    cpsr |= 0x10;                                         // user mode
    cpsr |= location.CPSR().Value() & 0x0600FE20;         // T, IT
    init.cpsr = cpsr;
    init.fpscr = (location.FPSCR().Value() & 0x07F70000) | (static_cast<u32>(rng.Next()) & 0xF000009F);
    init.cp15.uro = RandomWord(rng, image);
    init.cp15.uprw = RandomWord(rng, image);
    return init;
}

void StartJit(Dynarmic::A32::Jit& jit, JitCallbacks& cb, GuestMemory& memory, Cp15State& cp15,
              const InitialState& init, Outcome& outcome) {
    cp15 = init.cp15;
    cb.memory = &memory;
    cb.outcome = &outcome;
    cb.consumed = 0;
    cb.budget = 0;
    jit.ClearExclusiveState();
    std::copy(init.regs.begin(), init.regs.end(), jit.Regs().begin());
    std::copy(init.ext.begin(), init.ext.end(), jit.ExtRegs().begin());
    jit.SetCpsr(init.cpsr);
    jit.SetFpscr(init.fpscr);
}

/// Runs at least one more block (budget of one tick past what is consumed).
/// Random states can put guest code in a loop made only of LinkBlockFast
/// edges, which never checks the tick budget. Halt Dynarmic after a while.
struct Watchdog {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> running{false}, quit{false};
    std::atomic<bool> fired{false};
    std::atomic<std::uint64_t> current{0}; ///< Block under test, for reports.
    Dynarmic::A32::Jit* jit = nullptr;
    std::thread thread{[this] {
        std::uint64_t seen = 0;
        while (!quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000)); // Compiling junk blocks can take a while.
            const auto now = generation.load();
            if (running && now == seen && jit) {
                // A LinkBlockFast cycle never checks for halts, so a second
                // expiry means the reference is stuck for good: report and stop.
                if (fired) {
                    fmt::print(stderr, "stuck in Dynarmic at block {:016x}\n", current.load());
                    std::_Exit(3);
                }
                fired = true;
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            seen = now;
        }
    }};
    ~Watchdog() {
        quit = true;
        thread.join();
    }
};
Watchdog* g_watchdog = nullptr;

void ContinueJit(Dynarmic::A32::Jit& jit, JitCallbacks& cb, Outcome& outcome) {
    cb.budget = cb.consumed + 1;
    g_bad_run = &outcome.bad;
    g_watchdog->fired = false;
    ++g_watchdog->generation;
    g_watchdog->running = true;
    jit.Run();
    g_watchdog->running = false;
    ++g_watchdog->generation;
    if (g_watchdog->fired) {
        outcome.timed_out = true;
        outcome.bad = true;
    }
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined2);
    jit.ClearHalt();
    outcome.regs = jit.Regs();
    outcome.cpsr = jit.Cpsr();
    outcome.fpscr = jit.Fpscr();
    outcome.ext = jit.ExtRegs();
    outcome.ticks = cb.consumed;
}

void StartAot(AotContext& c, const Abi::Env& env, GuestMemory& memory, Cp15State& cp15,
              const InitialState& init, Outcome& outcome) {
    cp15 = init.cp15;
    c.memory = &memory;
    c.outcome = &outcome;
    c.consumed = 0;
    c.budget = 0;
    State& s = c.state;
    s = State{};
    s.env = &env;
    std::copy(init.regs.begin(), init.regs.end(), s.regs);
    std::copy(init.ext.begin(), init.ext.end(), s.ext_regs);
    s.SetCpsr(init.cpsr);
    s.SetFpscr(init.fpscr);
}

void ContinueAot(AotContext& c, Outcome& outcome) {
    State& s = c.state;
    c.budget = c.consumed + 1;

    // Dispatcher prologue: host FPCR from the guest mode, host FPSR from the guest flags.
    u64 saved_fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(saved_fpcr));
    asm volatile("msr fpcr, %0" ::"r"(u64(s.upper_location_descriptor & 0xFFFF0000)));
    Rt::WriteHostFpsr(s.fpsr);
    c.SyncTicksAfter();

    u64 descriptor = s.Descriptor();
    while (true) {
        if (s.halt)
            break;
        const auto* entry = Lookup(descriptor);
        if (!entry) {
            outcome.stopped_untranslated = true;
            break;
        }
        const u32 mode = s.upper_location_descriptor & 0xFFFF0000;
        descriptor = entry->fn(&s);
        if ((s.upper_location_descriptor & 0xFFFF0000) != mode)
            asm volatile("msr fpcr, %0" ::"r"(u64(s.upper_location_descriptor & 0xFFFF0000)));
        if (s.ticks <= 0)
            break;
    }
    c.SyncTicksBefore();
    s.fpsr = Rt::ReadHostFpsr();
    asm volatile("msr fpcr, %0" ::"r"(saved_fpcr));

    std::copy(std::begin(s.regs), std::end(s.regs), outcome.regs.begin());
    outcome.cpsr = s.Cpsr();
    outcome.fpscr = s.Fpscr();
    std::copy(std::begin(s.ext_regs), std::end(s.ext_regs), outcome.ext.begin());
    outcome.ticks = c.consumed;
}

u64 JitDescriptor(Dynarmic::A32::Jit& jit) {
    return Dynarmic::A32::LocationDescriptor{jit.Regs()[15], Dynarmic::A32::PSR{jit.Cpsr()},
                                             Dynarmic::A32::FPSCR{jit.Fpscr()}}
        .UniqueHash();
}

std::vector<std::string> Compare(const Outcome& jit, const Outcome& aot, GuestMemory& jm, GuestMemory& am) {
    std::vector<std::string> diffs;
    for (int i = 0; i < 16; ++i)
        if (jit.regs[i] != aot.regs[i])
            diffs.push_back(fmt::format("r{}: jit {:08x} aot {:08x}", i, jit.regs[i], aot.regs[i]));
    if (jit.cpsr != aot.cpsr)
        diffs.push_back(fmt::format("cpsr: jit {:08x} aot {:08x}", jit.cpsr, aot.cpsr));
    if (jit.fpscr != aot.fpscr)
        diffs.push_back(fmt::format("fpscr: jit {:08x} aot {:08x}", jit.fpscr, aot.fpscr));
    for (int i = 0; i < 64; ++i)
        if (jit.ext[i] != aot.ext[i])
            diffs.push_back(fmt::format("s{}: jit {:08x} aot {:08x}", i, jit.ext[i], aot.ext[i]));
    if (jit.events != aot.events) {
        const auto describe = [](const std::vector<Event>& events) {
            std::string text;
            for (const auto& e : events)
                text += e.what + "; ";
            return text;
        };
        diffs.push_back(fmt::format("events: jit [{}] aot [{}]", describe(jit.events), describe(aot.events)));
    }
    if (jit.ticks != aot.ticks)
        diffs.push_back(fmt::format("ticks: jit {} aot {}", jit.ticks, aot.ticks));
    std::vector<u32> keys;
    for (const auto& [k, _] : jm.pages)
        keys.push_back(k);
    for (const auto& [k, _] : am.pages)
        keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (const u32 k : keys) {
        const u8* a = jm.Page(k);
        const u8* b = am.Page(k);
        for (u32 i = 0; i < 4096; ++i) {
            if (a[i] != b[i]) {
                diffs.push_back(fmt::format("memory {:08x}: jit {:02x} aot {:02x}", (k << 12) + i, a[i], b[i]));
                break;
            }
        }
    }
    return diffs;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: dekui_aot_check <work> [--runs N] [--seed S] [--first D] [--limit N] [--verbose]\n", stderr);
        return 2;
    }
    const Image image{argv[1]};
    unsigned runs = 3;
    u64 seed = 1;
    u64 first = 0;
    std::size_t limit = SIZE_MAX;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--runs" && i + 1 < argc) runs = std::stoul(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        else if (arg == "--first" && i + 1 < argc) first = std::stoull(argv[++i], nullptr, 16);
        else if (arg == "--limit" && i + 1 < argc) limit = std::stoull(argv[++i]);
        else if (arg == "--verbose") verbose = true;
    }
    if (dekui_aot_module.code_sha256 != image.Sha256()) {
        fmt::print(stderr, "generated code is for {}, image is {}\n", dekui_aot_module.code_sha256, image.Sha256());
        return 1;
    }

    Cp15State jit_cp15, aot_cp15;
    JitCallbacks callbacks{image};
    Dynarmic::ExclusiveMonitor jit_monitor{1}, aot_monitor{1};
    Dynarmic::A32::UserConfig config;
    config.callbacks = &callbacks;
    for (std::size_t i = 0; i < config.coprocessors.size(); ++i)
        config.coprocessors[i] = std::make_shared<DynarmicCp15>(jit_cp15, i == 15);
    config.define_unpredictable_behaviour = true;
    config.global_monitor = &jit_monitor;
    config.processor_id = 0;
    Dynarmic::A32::Jit jit{config};
    callbacks.jit = &jit;
    Watchdog watchdog;
    watchdog.jit = &jit;
    g_watchdog = &watchdog;

    AotContext aot;
    aot.cp15 = &aot_cp15;
    aot.monitor = &aot_monitor;
    Abi::Env env{};
    env.read8 = AotRead<u8>;
    env.read16 = AotRead<u16>;
    env.read32 = AotRead<u32>;
    env.read64 = AotRead<u64>;
    env.write8 = AotWrite<u8>;
    env.write16 = AotWrite<u16>;
    env.write32 = AotWrite<u32>;
    env.write64 = AotWrite<u64>;
    env.exclusive_read8 = AotExclusiveRead<u8>;
    env.exclusive_read16 = AotExclusiveRead<u16>;
    env.exclusive_read32 = AotExclusiveRead<u32>;
    env.exclusive_read64 = AotExclusiveRead<u64>;
    env.exclusive_write8 = AotExclusiveWrite<u8>;
    env.exclusive_write16 = AotExclusiveWrite<u16>;
    env.exclusive_write32 = AotExclusiveWrite<u32>;
    env.exclusive_write64 = AotExclusiveWrite<u64>;
    env.call_svc = AotSvc;
    env.exception_raised = AotException;
    env.coproc_internal = AotCoprocUnsupported;
    env.coproc_send_one = AotCoprocSendOne;
    env.coproc_send_two = AotCoprocUnsupported2;
    env.coproc_get_one = AotCoprocGetOne;
    env.coproc_get_two = AotCoprocUnsupported3;
    env.coproc_load = AotCoprocUnsupported4;
    env.coproc_store = AotCoprocUnsupported4;

    std::size_t tested = 0, passed = 0, failed = 0, skipped = 0, failed_blocks = 0, timed_out = 0;
    std::map<std::string, std::size_t> failure_kinds;
    const auto* begin = dekui_aot_module.blocks;
    const auto* end = begin + dekui_aot_module.block_count;
    for (const auto* entry = begin; entry != end && tested < limit; ++entry) {
        if (entry->descriptor < first)
            continue;
        bool block_failed = false;
        g_watchdog->current = entry->descriptor;
        if (verbose)
            fmt::print(stderr, "block {:016x}\n", entry->descriptor);
        else if ((entry - begin) % 10000 == 0)
            fmt::print(stderr, "progress {} {:016x} failed {}\n", entry - begin, entry->descriptor, failed);
        for (unsigned run = 0; run < runs; ++run) {
            Rng rng{SplitMix(seed ^ entry->descriptor) + run};
            const InitialState init = MakeState(rng, image, entry->descriptor);
            const u64 memory_seed = rng.Next();
            GuestMemory jit_memory{image, memory_seed}, aot_memory{image, memory_seed};
            env.page_table = aot_memory.page_table.data();
            jit_monitor.Clear();
            aot_monitor.Clear();
            Outcome j, a;
            StartJit(jit, callbacks, jit_memory, jit_cp15, init, j);
            StartAot(aot, env, aot_memory, aot_cp15, init, a);
            ContinueJit(jit, callbacks, j);
            if (!j.timed_out)
                ContinueAot(aot, a);
            // The engines may stop at different block boundaries (Dynarmic only
            // links blocks it has already compiled). Advance whichever is
            // behind until both stand at the same place.
            for (int step = 0; step < 512 && !j.bad && !a.bad && !a.stopped_untranslated; ++step) {
                const bool same_place = JitDescriptor(jit) == aot.state.Descriptor() && j.ticks == a.ticks;
                if (same_place)
                    break;
                // An engine that stopped on an event is done; the other may catch up.
                if (j.ticks <= a.ticks && j.events.empty())
                    ContinueJit(jit, callbacks, j);
                else if (a.ticks < j.ticks && a.events.empty())
                    ContinueAot(aot, a);
                else
                    break;
            }
            ++tested;
            if (j.timed_out)
                ++timed_out;
            if (j.bad || a.bad || a.stopped_untranslated) {
                ++skipped;
                continue;
            }
            const auto diffs = Compare(j, a, jit_memory, aot_memory);
            if (diffs.empty()) {
                ++passed;
                continue;
            }
            ++failed;
            block_failed = true;
            for (const auto& d : diffs)
                ++failure_kinds[d.substr(0, d.find(':'))];
            if (verbose || failed_blocks < 20) {
                fmt::print("FAIL {:08x}:{:08x} run {}\n", u32(entry->descriptor), u32(entry->descriptor >> 32), run);
                for (const auto& d : diffs)
                    fmt::print("    {}\n", d);
            }
        }
        failed_blocks += block_failed;
    }
    fmt::print("tested {} runs: {} passed, {} failed ({} blocks), {} skipped ({} endless loops)\n",
               tested, passed, failed, failed_blocks, skipped, timed_out);
    for (const auto& [kind, count] : failure_kinds)
        fmt::print("  {}: {}\n", kind, count);
    return failed == 0 ? 0 : 1;
}
