// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef ENABLE_MM3D_PRESENTATION
#include "mm3d/camera_live.h"
#include "mm3d/wide_view.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cryptopp/sha.h>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/file_util.h"
#include "common/hitch_trace.h"
#include "core/arm/aot/runtime.h"
#include "core/arm/aot_coverage.h"
#include "core/memory.h"

#if defined(__aarch64__)
#include "core/arm/aot/block_support.h"
#endif

#ifdef DEKUI_AOT_LINKED
extern "C" const DekuiAot::Abi::Module dekui_aot_module;
#endif

namespace Core::Aot {

namespace {

using DekuiAot::Abi::BlockFn;
using DekuiAot::Abi::Module;
using DekuiAot::Abi::State;

constexpr u64 MM3DProgramId = 0x0004000000125500ULL;

/// Process-wide gate. The emulator runs every guest CPU on one host thread,
/// so this needs no locking.
struct Gate {
    bool enabled = false;
    std::vector<std::pair<u32, u32>> invalid; ///< [start, end) ranges.
};

Gate& GetGate() {
    static Gate gate;
    return gate;
}

std::string Sha256(std::span<const u8> data) {
    u8 digest[CryptoPP::SHA256::DIGESTSIZE];
    CryptoPP::SHA256().CalculateDigest(digest, data.data(), data.size());
    std::string text;
    for (const u8 byte : digest)
        text += fmt::format("{:02x}", byte);
    return text;
}

} // namespace

const Module* LinkedModule() {
#if defined(DEKUI_AOT_LINKED) && defined(__aarch64__)
    return &dekui_aot_module;
#else
    return nullptr;
#endif
}

void OnCodeLoaded(u64 program_id, std::span<const u8> image) {
    auto& gate = GetGate();
    gate.enabled = false;
    gate.invalid.clear();
    const Module* module = LinkedModule();
    if (!module || program_id != MM3DProgramId)
        return;
    if (module->abi_version != DekuiAot::Abi::Version) {
        LOG_ERROR(Core_ARM11, "AOT: generated code uses ABI {}, runtime expects {}",
                  module->abi_version, DekuiAot::Abi::Version);
        return;
    }
    if (image.size() < module->code_size) {
        LOG_WARNING(Core_ARM11, "AOT: loaded image is smaller than the translated one; disabled");
        return;
    }
    const std::string hash = Sha256(image.first(module->code_size));
    if (hash != module->code_sha256) {
        LOG_WARNING(Core_ARM11, "AOT: loaded code {} does not match translated code {}; disabled",
                    hash, module->code_sha256);
        return;
    }
    gate.enabled = true;
    LOG_INFO(Core_ARM11, "AOT: {} translated blocks enabled", module->block_count);
#ifdef ENABLE_MM3D_PRESENTATION
    // A code substitution newer than some generated modules (the immersive
    // view's restore hook): its block runs on the fallback CPU, which applies
    // the substitution, until the module is regenerated with it.
    for (const u32 pc : MM3D::WideView::RestorePCs)
        Invalidate(pc, 4);
    for (const auto& site : MM3D::WideView::LookAtSites)
        Invalidate(site.block, site.pc + 4 - site.block);
    // The auxiliary projection load follows a call, so it starts its block.
    for (const u32 pc : MM3D::CameraLive::AuxiliaryProjectionPCs)
        Invalidate(pc, 4);
#endif
    // Blocks the interpreter still has to run show where translation is missing.
    AotCoverage::EnableIn(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "aot-coverage");
}

void Invalidate(u32 start, std::size_t length) {
    auto& gate = GetGate();
    if (!gate.enabled || length == 0)
        return;
    const u64 end = std::min<u64>(u64(start) + length, 0x100000000ull);
    gate.invalid.emplace_back(start, static_cast<u32>(end - 1) + 1);
    LOG_INFO(Core_ARM11, "AOT: guest invalidated {:08x}+{:x}; those blocks now use the fallback CPU",
             start, length);
}

void InvalidateAll() {
    auto& gate = GetGate();
    if (!gate.enabled)
        return;
    // The image cannot be re-verified once the game has written its data
    // segment, so stop trusting translated code entirely.
    gate.enabled = false;
    LOG_WARNING(Core_ARM11, "AOT: guest invalidated the whole instruction cache; AOT disabled");
}

BlockFn Lookup(u64 descriptor) {
    const auto& gate = GetGate();
    const Module* module = LinkedModule();
    if (!gate.enabled || !module)
        return nullptr;
    const auto* begin = module->blocks;
    const auto* end = begin + module->block_count;
    const auto* it = std::lower_bound(begin, end, descriptor, [](const auto& entry, u64 d) {
        return entry.descriptor < d;
    });
    if (it == end || it->descriptor != descriptor)
        return nullptr;
    const u32 pc = static_cast<u32>(descriptor);
    for (const auto& [start, stop] : gate.invalid)
        if (pc >= start && (stop == 0 || pc < stop))
            return nullptr;
    return it->fn;
}

// ---------------------------------------------------------------------------

#if defined(__aarch64__)

namespace {

Runner& RunnerOf(State* s) {
    return *static_cast<Runner*>(s->host_context);
}

// Guest profile for slow frames. Translated blocks chain into each other, so
// one dispatch can run a whole loop; counting dispatches would say nothing
// about time. Instead a sampler thread counts `sample_due` up every 0.5 ms,
// and the next dispatch (or SVC) charges those samples to the code that was
// running: the entry of the current run of chained blocks. A run that loops
// for milliseconds gets all its samples. That costs one load per dispatch.
// SVC time is measured exactly instead (Phase::AotSvc).
constexpr auto SampleInterval = std::chrono::microseconds(500);
std::atomic<u32> sample_due{0};

struct Profile {
    /// Key: guest PC | Thumb bit.
    struct Entry {
        u32 key = 0;
        u32 samples = 0;
    };
    std::array<Entry, 2048> table{};
    std::vector<u16> used;
    u32 samples = 0, lost = 0;
    Memory::MemorySystem* memory = nullptr;
    /// Guessed function starts, kept across frames.
    std::unordered_map<u32, u32> function_of;

    void Hit(u32 key, u32 count) {
        samples += count;
        u32 slot = (key * 0x9E3779B1u) >> 21;
        for (u32 probe = 0; probe < 64; ++probe, slot = (slot + 1) % table.size()) {
            Entry& entry = table[slot];
            if (entry.key == key) {
                entry.samples += count;
                return;
            }
            if (entry.key == 0) {
                entry = {key, count};
                used.push_back(static_cast<u16>(slot));
                return;
            }
        }
        lost += count;
    }

    void Reset() {
        for (const u16 slot : used)
            table[slot] = {};
        used.clear();
        samples = lost = 0;
    }

    /// The function containing `key`: the nearest preceding push of LR,
    /// which is how MM3D's functions begin. Leaf functions without one are
    /// merged into the function before them.
    u32 FunctionOf(u32 key) {
        if (const auto it = function_of.find(key); it != function_of.end())
            return it->second;
        const bool thumb = key & 1;
        const u32 pc = key & ~1u;
        u32 start = pc;
        for (u32 at = pc & ~3u, steps = 0; at >= 4 && steps < 0x1000; at -= 4, ++steps) {
            const std::optional<u32> word = memory->Read32OrNullopt(at);
            if (!word)
                break;
            if (!thumb) {
                if ((*word & 0xFFFF4000) == 0xE92D4000 || *word == 0xE52DE004) {
                    start = at;
                    break;
                }
                continue;
            }
            // Halfwords at `at + 2` then `at`: PUSH {..., LR} or PUSH.W {..., LR}.
            const u16 high = static_cast<u16>(*word >> 16), low = static_cast<u16>(*word);
            if (at + 2 <= pc && (high & 0xFF00) == 0xB500) {
                start = at + 2;
                break;
            }
            if ((low & 0xFF00) == 0xB500 || (low == 0xE92D && (high & 0x4000))) {
                start = at;
                break;
            }
        }
        start |= key & 1;
        function_of.emplace(key, start);
        return start;
    }
};

Profile& GetProfile() {
    static Profile profile;
    return profile;
}

bool ProfilingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DEKUI_HITCH_PROFILE");
        return !value || std::string_view{value} != "0";
    }();
    return enabled;
}

void StartSampler() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            // While no guest code runs (paused, menus on the host), back off.
            while (true) {
                const bool idle = sample_due.load(std::memory_order_relaxed) > 400;
                std::this_thread::sleep_for(idle ? std::chrono::milliseconds(50)
                                                 : SampleInterval);
                sample_due.fetch_add(1, std::memory_order_relaxed);
            }
        }).detach();
    });
}

u32 SampleKey(const State& s, u32 pc) {
    return pc | (s.upper_location_descriptor & 1);
}

std::string ReportProfile(const Common::HitchTrace::FrameTotals& frame) {
    Profile& profile = GetProfile();
    if (!frame.slow || profile.samples == 0 || !profile.memory) {
        profile.Reset();
        return {};
    }
    using Common::HitchTrace::Phase;
    const double aot_ms = frame.nanoseconds[static_cast<std::size_t>(Phase::Aot)] / 1e6;
    const double svc_ms = frame.nanoseconds[static_cast<std::size_t>(Phase::AotSvc)] / 1e6;
    const double code_ms = std::max(aot_ms - svc_ms, 0.0);

    struct Function {
        u32 start = 0, samples = 0;
        std::vector<std::pair<u32, u32>> pcs; ///< (samples, key)
    };
    std::vector<Function> functions;
    std::unordered_map<u32, std::size_t> index;
    for (const u16 slot : profile.used) {
        const auto& entry = profile.table[slot];
        const u32 start = profile.FunctionOf(entry.key);
        auto [it, inserted] = index.emplace(start, functions.size());
        if (inserted)
            functions.push_back({start});
        Function& function = functions[it->second];
        function.samples += entry.samples;
        function.pcs.emplace_back(entry.samples, entry.key);
    }
    std::sort(functions.begin(), functions.end(),
              [](const Function& a, const Function& b) { return a.samples > b.samples; });

    const double total = profile.samples;
    std::string line = fmt::format(
        "Hitch guest: aot {:.1f} ms = code {:.1f} + svc {:.1f}; {} samples in {} functions",
        aot_ms, code_ms, svc_ms, profile.samples, functions.size());
    if (profile.lost)
        line += fmt::format(" ({} unsorted)", profile.lost);
    for (std::size_t i = 0; i < std::min<std::size_t>(functions.size(), 8); ++i) {
        Function& function = functions[i];
        const double share = function.samples / total;
        line += fmt::format(" | fn {:08x} {:.0f}% ~{:.1f} ms", function.start, share * 100,
                            share * code_ms);
        std::sort(function.pcs.rbegin(), function.pcs.rend());
        const std::size_t shown = std::min<std::size_t>(function.pcs.size(), 3);
        for (std::size_t j = 0; j < shown; ++j)
            line += fmt::format("{}{:08x} {:.0f}%", j == 0 ? " (at " : ", ", function.pcs[j].second,
                                function.pcs[j].first / total * 100);
        if (shown)
            line += ")";
    }
    profile.Reset();
    return line;
}

template <typename T>
T ReadSlow(State* s, u32 vaddr) {
    auto& memory = RunnerOf(s).memory;
    if constexpr (sizeof(T) == 1)
        return memory.Read8(vaddr);
    else if constexpr (sizeof(T) == 2)
        return memory.Read16(vaddr);
    else if constexpr (sizeof(T) == 4)
        return memory.Read32(vaddr);
    else
        return memory.Read64(vaddr);
}

template <typename T>
void WriteSlow(State* s, u32 vaddr, T value) {
    auto& memory = RunnerOf(s).memory;
    if constexpr (sizeof(T) == 1)
        memory.Write8(vaddr, value);
    else if constexpr (sizeof(T) == 2)
        memory.Write16(vaddr, value);
    else if constexpr (sizeof(T) == 4)
        memory.Write32(vaddr, value);
    else
        memory.Write64(vaddr, value);
}

template <typename T>
T ExclusiveRead(State* s, u32 vaddr) {
    return static_cast<T>(RunnerOf(s).host.ExclusiveRead(vaddr, sizeof(T)));
}

template <typename T>
u32 ExclusiveWrite(State* s, u32 vaddr, T value) {
    return RunnerOf(s).host.ExclusiveWrite(vaddr, value, sizeof(T)) ? 0 : 1;
}

void CallSVC(State* s, u32 swi) {
    Runner& runner = RunnerOf(s);
    // Samples due now belong to the guest code that led to this SVC.
    if (sample_due.load(std::memory_order_relaxed)) [[unlikely]] {
        GetProfile().Hit(SampleKey(*s, s->regs[15]),
                         sample_due.exchange(0, std::memory_order_relaxed));
    }
    const Common::HitchTrace::Scope hitch_scope{Common::HitchTrace::Phase::AotSvc};
    runner.BeforeCallback();
    runner.host.CallSVC(swi);
    runner.AfterCallback();
    // Time inside the SVC is measured exactly, not sampled.
    sample_due.store(0, std::memory_order_relaxed);
}

void ExceptionRaised(State* s, u32 pc, u32 exception) {
    Runner& runner = RunnerOf(s);
    runner.BeforeCallback();
    runner.host.ExceptionRaised(pc, exception);
    runner.AfterCallback();
}

// Dynarmic CoprocInfo bytes: [0] coprocessor, [1] two, [2] opc1, [3] CRn, [4] CRm, [5] opc2.
u32* Cp15(State* s, bool get, u64 info) {
    const auto b = [&](int i) { return static_cast<unsigned>((info >> (8 * i)) & 0xFF); };
    u32* reg = nullptr;
    if (b(0) == 15)
        reg = RunnerOf(s).host.Cp15Register(get, b(1) != 0, b(2), b(3), b(4), b(5));
    if (!reg)
        LOG_ERROR(Core_ARM11, "AOT: unsupported coprocessor access {:016x} at {:08x}", info,
                  s->regs[15]);
    return reg;
}

void CoprocSendOne(State* s, u64 info, u32 value) {
    if (u32* reg = Cp15(s, false, info))
        *reg = value;
}

u32 CoprocGetOne(State* s, u64 info) {
    const u32* reg = Cp15(s, true, info);
    return reg ? *reg : 0;
}

void CoprocUnsupported(State* s, u64 info) {
    LOG_ERROR(Core_ARM11, "AOT: unsupported coprocessor operation {:016x} at {:08x}", info,
              s->regs[15]);
}
void CoprocUnsupportedTwo(State* s, u64 info, u32, u32) {
    CoprocUnsupported(s, info);
}
u64 CoprocUnsupportedGetTwo(State* s, u64 info) {
    CoprocUnsupported(s, info);
    return 0;
}
void CoprocUnsupportedAddress(State* s, u64 info, u32) {
    CoprocUnsupported(s, info);
}

u64 ReadFpcr() {
    u64 value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}
void WriteFpcr(u64 value) {
    asm volatile("msr fpcr, %0" ::"r"(value));
}

} // namespace

Runner::Runner(Memory::MemorySystem& memory_, Host& host_) : memory(memory_), host(host_) {
    env.read8 = ReadSlow<u8>;
    env.read16 = ReadSlow<u16>;
    env.read32 = ReadSlow<u32>;
    env.read64 = ReadSlow<u64>;
    env.write8 = WriteSlow<u8>;
    env.write16 = WriteSlow<u16>;
    env.write32 = WriteSlow<u32>;
    env.write64 = WriteSlow<u64>;
    env.exclusive_read8 = ExclusiveRead<u8>;
    env.exclusive_read16 = ExclusiveRead<u16>;
    env.exclusive_read32 = ExclusiveRead<u32>;
    env.exclusive_read64 = ExclusiveRead<u64>;
    env.exclusive_write8 = ExclusiveWrite<u8>;
    env.exclusive_write16 = ExclusiveWrite<u16>;
    env.exclusive_write32 = ExclusiveWrite<u32>;
    env.exclusive_write64 = ExclusiveWrite<u64>;
    env.call_svc = CallSVC;
    env.exception_raised = ExceptionRaised;
    env.coproc_internal = CoprocUnsupported;
    env.coproc_send_one = CoprocSendOne;
    env.coproc_send_two = CoprocUnsupportedTwo;
    env.coproc_get_one = CoprocGetOne;
    env.coproc_get_two = CoprocUnsupportedGetTwo;
    env.coproc_load = CoprocUnsupportedAddress;
    env.coproc_store = CoprocUnsupportedAddress;
    state.env = &env;
    state.host_context = this;
}

// Tick and FPSR handling around callbacks follows Dynarmic's arm64 backend:
// report what ran so far, let the emulator act, then refetch the budget.
void Runner::BeforeCallback() {
    host.AddTicks(static_cast<u64>(static_cast<s64>(cycles_to_run) - state.ticks));
    state.fpsr = DekuiAot::Rt::ReadHostFpsr();
    host.Store(state);
}

void Runner::AfterCallback() {
    host.Load(state);
    DekuiAot::Rt::WriteHostFpsr(state.fpsr);
    cycles_to_run = host.TicksRemaining();
    state.ticks = static_cast<s64>(cycles_to_run);
    if (host.HaltRequested())
        state.halt = 1;
}

Runner::~Runner() {
    Profile& profile = GetProfile();
    if (profile.memory == &memory) {
        profile.Reset();
        profile.function_of.clear();
        profile.memory = nullptr;
    }
}

bool Runner::Run(u64 descriptor, Memory::PageTable& page_table) {
    BlockFn fn = Lookup(descriptor);
    if (!fn)
        return false;
    const Common::HitchTrace::Scope hitch_scope{Common::HitchTrace::Phase::Aot};

    host.Load(state);
    state.halt = 0;
    env.page_table = page_table.GetPointerArray().data();

    if (ProfilingEnabled()) {
        Profile& profile = GetProfile();
        if (profile.memory != &memory) {
            profile.Reset();
            profile.function_of.clear();
            profile.memory = &memory;
            Common::HitchTrace::AddReporter(ReportProfile);
            StartSampler();
        }
        // Samples raised before this run belong to other code.
        sample_due.store(0, std::memory_order_relaxed);
    }

    const u64 saved_fpcr = ReadFpcr();
    u32 mode = state.upper_location_descriptor & 0xFFFF0000;
    WriteFpcr(mode);
    DekuiAot::Rt::WriteHostFpsr(state.fpsr);
    cycles_to_run = host.TicksRemaining();
    state.ticks = static_cast<s64>(cycles_to_run);

    u64 entry = descriptor;
    while (true) {
        const u64 next = fn(&state);
        // Only set while profiling.
        if (sample_due.load(std::memory_order_relaxed)) [[unlikely]] {
            GetProfile().Hit(static_cast<u32>(entry) | static_cast<u32>(entry >> 32 & 1),
                             sample_due.exchange(0, std::memory_order_relaxed));
        }
        if (state.halt || state.ticks <= 0)
            break;
        const u32 next_mode = state.upper_location_descriptor & 0xFFFF0000;
        if (next_mode != mode) {
            mode = next_mode;
            WriteFpcr(mode);
        }
        fn = Lookup(next);
        if (!fn)
            break;
        entry = next;
    }

    host.AddTicks(static_cast<u64>(static_cast<s64>(cycles_to_run) - state.ticks));
    state.fpsr = DekuiAot::Rt::ReadHostFpsr();
    WriteFpcr(saved_fpcr);
    host.Store(state);
    return true;
}

#else

Runner::Runner(Memory::MemorySystem& memory_, Host& host_) : memory(memory_), host(host_) {}
Runner::~Runner() = default;
void Runner::BeforeCallback() {}
void Runner::AfterCallback() {}
bool Runner::Run(u64, Memory::PageTable&) {
    return false;
}

#endif

} // namespace Core::Aot
