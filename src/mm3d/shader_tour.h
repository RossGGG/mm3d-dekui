// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>
#include "common/logging/log.h"

// Shader tour: with DEKUI_SHADER_TOUR=1 the game warps Link to every scene
// entrance, then as Deku, Goron, Zora and Fierce Deity Link through the main
// areas, while scripted input walks, attacks, rolls, charges and turns. Every
// shader and pipeline drawn on the way lands in the disk shader cache, which
// tools/mm3d/shader_tour.py exports for other devices. Run it on a copy of the
// user folder: it loads file 1, changes form, health and the day in memory.
namespace MM3D::ShaderTour {
using U32 = std::uint32_t;

// USA Restoration addresses (see death_penalties.h, simple_cheats.h).
constexpr U32 UiContext = 0x72f5c0, GameOffset = 0x16c;   // current GameState
constexpr U32 StateOffset = 0x13c, SceneOffset = 0x148;    // (type << 8 | status), scene id
constexpr U32 NextEntrance = 0xc52e, TransitionTrigger = 0xc529;
constexpr U32 TransitionType = 0xc534, TransitionUnknown = 0xc535;
constexpr U32 SaveMask = 0x7751e0, SaveIsNight = 0x7751e4, SaveDay = 0x7751ec;
constexpr U32 SaveTime = 0x7751f8, SaveForm = 0x7751fe;
constexpr U32 MaxHealth = 0x775312, Health = 0x775314;
constexpr U32 PlayType = 4, FileSelectType = 6;

// Entrance groups 0x00..0x6F (entrance = group << 9, spawn 0). These do not
// load a scene (from MM3D-PC's tour, same entrance table on 3DS). In 0x1D's
// scene (0x20) the game freezes on a black screen after a few seconds.
constexpr std::array<U32, 7> Broken{0x0b, 0x0c, 0x0d, 0x0f, 0x1d, 0x2e, 0x37};
// Main areas for the other forms and for night: Termina Field, the four Clock Town areas,
// Southern Swamp, Deku Palace, Woodfall, Mountain Village, Snowhead, Great Bay
// Coast, Zora Hall, Ikana Canyon, Romani Ranch, Lost Woods.
constexpr std::array<U32, 15> FormEntrances{0x5400, 0xd800, 0xd600, 0xd000, 0xd400,
                                            0x8400, 0x8600, 0x5000, 0x9a00, 0x9400,
                                            0xb200, 0x6800, 0x4600, 0x2000, 0x6400};
enum Form : U32 { FierceDeity = 0, Goron = 1, Zora = 2, Deku = 3, Human = 4 };
constexpr U32 MaskFor(U32 form) {
    return form == Deku ? 0x18 : form == Goron ? 0x16 : form == Zora ? 0x17
         : form == FierceDeity ? 0x15 : 0;
}
constexpr const char* FormName(U32 form) {
    return form == Deku ? "Deku" : form == Goron ? "Goron" : form == Zora ? "Zora"
         : form == FierceDeity ? "Fierce Deity" : "Human";
}

struct Step { U32 form, entrance; bool night = false; };
inline const char* StepName(const Step& step) {
    return step.night ? "Human at night" : FormName(step.form);
}
inline std::vector<Step> Plan() {
    std::vector<Step> plan;
    for (U32 group = 0; group < 0x70; ++group) {
        bool broken = false;
        for (U32 b : Broken) broken |= b == group;
        if (!broken) plan.push_back({Human, group << 9});
    }
    for (U32 form : {Deku, Goron, Zora, FierceDeity})
        for (U32 entrance : FormEntrances) plan.push_back({form, entrance});
    // Night: fog, sky and night-only actors.
    for (U32 entrance : FormEntrances) plan.push_back({Human, entrance, true});
    return plan;
}

// Emulated time: the cheat engine ticks every 50,000,000 ARM11 cycles.
constexpr double TickSeconds = 50'000'000.0 / 268'111'856.0;
// HID pad updates run at 234 Hz.
constexpr double PadSeconds = 1.0 / 234.0;

enum class Phase : U32 { Off, Boot, Settle, Warp, Loading, Dwell, Done };
struct State {
    bool configured = false, seen_file_select = false;
    double dwell = 12.0, form_dwell = 10.0, timeout = 25.0;
    std::vector<Step> plan;
    std::size_t index = 0, visited = 0, failed = 0, start = 0;
    double clock = 0, phase_start = 0;
    U32 last_type = ~0u, from_scene = ~0u;
};
inline State state;
inline std::atomic<Phase> phase{Phase::Off};
inline std::atomic<U32> pad_count{0};

inline double EnvSeconds(const char* name, double fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atof(v) : fallback;
}
inline void Configure() {
    state.configured = true;
    const char* on = std::getenv("DEKUI_SHADER_TOUR");
    if (!on || std::string(on) != "1") return;
    state.plan = Plan();
    state.dwell = EnvSeconds("DEKUI_SHADER_TOUR_SECONDS", 12.0);
    state.form_dwell = EnvSeconds("DEKUI_SHADER_TOUR_FORM_SECONDS", 10.0);
    state.start = static_cast<std::size_t>(EnvSeconds("DEKUI_SHADER_TOUR_START", 0));
    state.index = state.start;
    phase = Phase::Boot;
    LOG_INFO(Frontend, "Shader tour: {} steps, starting at {}, {} s per scene",
             state.plan.size(), state.start, state.dwell);
}

template <class M> std::optional<U32> Get(M& m, U32 at, unsigned bytes) {
    auto w = m.Read(at & ~3u);
    if (!w) return std::nullopt;
    return (*w >> ((at & 3) * 8)) & (bytes == 1 ? 0xffu : 0xffffu);
}
template <class M> void Set(M& m, U32 at, U32 value, unsigned bytes) {
    const U32 shift = (at & 3) * 8, mask = (bytes == 1 ? 0xffu : 0xffffu) << shift;
    if (auto w = m.Read(at & ~3u)) m.Write(at & ~3u, (*w & ~mask) | ((value << shift) & mask));
}
inline bool Pointer(std::optional<U32> p) {
    return p && *p >= 0x100000 && *p < 0x3ffff000 && !(*p & 3);
}
template <class M> std::optional<U32> Game(M& m) {
    auto ui = m.Read(UiContext);
    if (!Pointer(ui)) return std::nullopt;
    auto game = m.Read(*ui + GameOffset);
    if (!Pointer(game)) return std::nullopt;
    return game;
}

template <class M> void Begin(M& m, U32 game, const Step& step) {
    // Keep Link alive and the moon away; both would end the tour.
    if (auto max = Get(m, MaxHealth, 2)) Set(m, Health, *max, 2);
    if (auto day = m.Read(SaveDay); day && *day >= 3) m.Write(SaveDay, 1);
    // Link spawns in the saved form when the scene loads.
    Set(m, SaveForm, step.form, 1);
    Set(m, SaveMask, MaskFor(step.form), 1);
    if (step.night) {
        Set(m, SaveTime, 0x0000, 2); // midnight; the scene loads with night settings
        Set(m, SaveIsNight, 1, 1);
    }
    // As the native void respawn: entrance, trigger "start", fade to black.
    Set(m, game + NextEntrance, step.entrance, 2);
    Set(m, game + TransitionType, 2, 1);
    Set(m, game + TransitionUnknown, 0, 1);
    Set(m, game + TransitionTrigger, 20, 1);
    state.from_scene = Get(m, game + SceneOffset, 2).value_or(~0u);
}

/// Runs from the cheat engine's periodic callback while MM3D is running.
template <class M> void Tick(M& m, bool mm3d) {
    if (!state.configured) Configure();
    const Phase p = phase;
    if (p == Phase::Off || p == Phase::Done || !mm3d) return;
    state.clock += TickSeconds;
    const double elapsed = state.clock - state.phase_start;
    auto to = [](Phase next) { phase = next; state.phase_start = state.clock; };

    const auto game = Game(m);
    const auto raw = game ? Get(m, *game + StateOffset, 2) : std::nullopt;
    const U32 type = raw ? (*raw >> 8) & 0xff : ~0u, status = raw ? *raw & 0xff : ~0u;
    if (type != state.last_type) {
        LOG_INFO(Frontend, "Shader tour: game state type {} (status {})", type, status);
        state.last_type = type;
    }
    if (type == FileSelectType) state.seen_file_select = true;
    const bool playing = type == PlayType && status == 2;
    if (playing && game) {
        if (auto max = Get(m, MaxHealth, 2)) Set(m, Health, *max, 2);
    }

    switch (p) {
    case Phase::Boot:
        // Input presses A through the title screen and file select (file 1).
        if (playing && state.seen_file_select) {
            LOG_INFO(Frontend, "Shader tour: file loaded, scene {:#x}",
                     Get(m, *game + SceneOffset, 2).value_or(0));
            to(Phase::Settle);
        }
        break;
    case Phase::Settle:
        if (elapsed > 4.0) to(Phase::Warp);
        break;
    case Phase::Warp:
        if (state.index >= state.plan.size()) {
            LOG_INFO(Frontend, "Shader tour: done, {} scenes visited, {} failed",
                     state.visited, state.failed);
            to(Phase::Done);
            break;
        }
        if (playing && Get(m, *game + TransitionTrigger, 1) == 0u) {
            Begin(m, *game, state.plan[state.index]);
            to(Phase::Loading);
        } else if (type == PlayType && elapsed > 8.0 && elapsed < 9.0) {
            // A transition that never finishes (a black screen): start the
            // next one over it.
            LOG_WARNING(Frontend, "Shader tour: forcing the warp (status {}, trigger {})", status,
                        Get(m, *game + TransitionTrigger, 1).value_or(~0u));
            Begin(m, *game, state.plan[state.index]);
            to(Phase::Loading);
        } else if (elapsed > state.timeout) {
            LOG_WARNING(Frontend,
                        "Shader tour: stuck outside gameplay (state type {}, status {}); stopping",
                        type, status);
            to(Phase::Done);
        }
        break;
    case Phase::Loading: {
        const auto& step = state.plan[state.index];
        const auto scene = playing ? Get(m, *game + SceneOffset, 2) : std::nullopt;
        const bool arrived = playing && elapsed > 1.5 &&
                             Get(m, *game + TransitionTrigger, 1) == 0u && scene &&
                             (*scene != state.from_scene || elapsed > 6.0);
        if (arrived) {
            LOG_INFO(Frontend, "Shader tour: [{}/{}] {} entrance {:#06x} -> scene {:#04x} ({:.1f} s)",
                     state.index + 1, state.plan.size(), StepName(step), step.entrance,
                     *scene, elapsed);
            ++state.visited;
            to(Phase::Dwell);
        } else if (elapsed > state.timeout) {
            LOG_WARNING(Frontend,
                        "Shader tour: [{}/{}] {} entrance {:#06x} did not load (state type {}, "
                        "status {}, trigger {})",
                        state.index + 1, state.plan.size(), StepName(step), step.entrance,
                        type, status,
                        game ? Get(m, *game + TransitionTrigger, 1).value_or(~0u) : ~0u);
            ++state.failed;
            ++state.index;
            to(Phase::Warp);
        }
        break;
    }
    case Phase::Dwell: {
        const auto& step = state.plan[state.index];
        if (elapsed > (step.form == Human ? state.dwell : state.form_dwell)) {
            ++state.index;
            to(Phase::Warp);
        }
        break;
    }
    default:
        break;
    }
}

/// Scripted input, applied in HID's pad update. Bits: A=0, B=1, Start=3.
template <class Axis> void FilterPad(std::uint32_t& buttons, Axis& x, Axis& y) {
    const Phase p = phase;
    if (p == Phase::Off) return;
    const U32 n = pad_count.fetch_add(1);
    const double t = n * PadSeconds;
    if (p == Phase::Boot) {
        // Tap A for 0.15 s every 1.2 s: title screen, file 1, confirm.
        buttons = std::fmod(t, 1.2) < 0.15 ? 1u : 0u;
        x = y = 0;
        return;
    }
    if (p != Phase::Dwell) {
        if (p != Phase::Done) { buttons = 0; x = y = 0; }
        return;
    }
    // A 3 s cycle: walk, attack, roll, charge a spin attack, turn.
    constexpr Axis Full = 0x9a;
    const double c = std::fmod(t, 3.0);
    const bool odd = static_cast<U32>(t / 3.0) & 1;
    buttons = 0; x = 0; y = 0;
    if (c < 0.6) y = Full;                                   // walk forward
    else if (c < 0.7) buttons = 2;                           // B: attack
    else if (c >= 0.8 && c < 0.9) { buttons = 1; y = Full; } // A + forward: roll / jump
    else if (c >= 1.0 && c < 2.0) buttons = 2;               // hold B: charge, then release
    else if (c >= 2.1 && c < 2.5) x = odd ? Full : -Full;    // turn
}
} // namespace MM3D::ShaderTour
