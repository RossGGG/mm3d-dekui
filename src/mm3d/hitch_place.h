// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <fmt/format.h>
#include "common/hitch_trace.h"
#include "mm3d/shader_tour.h"

// Tells Hitch lines where the game is, so stutter bursts can be tied to
// places. Ticked with the cheat engine (about 5 times a second); the Hitch
// line only changes when the place does. Scene ids are the ones the shader
// tour logs ("entrance 0x1200 -> scene 0x0f"). The current entrance is not
// reported: the SaveData word before the mask keeps the last saved entrance,
// not the one that loaded this scene.
namespace MM3D::HitchPlace {
using U32 = std::uint32_t;

inline U32 last_key = ~0u;

template <class M> void Tick(M& m, bool mm3d) {
    using namespace ShaderTour;
    U32 key = 0;
    std::string place;
    if (!mm3d) {
        key = 1;
    } else if (const auto game = Game(m); !game) {
        key = 2;
        place = "no game state";
    } else {
        const U32 type = (Get(m, *game + StateOffset, 2).value_or(0) >> 8) & 0xff;
        if (type != PlayType) {
            key = 0x80000000u | type;
            place = fmt::format("game state {}", type);
        } else {
            const U32 scene = Get(m, *game + SceneOffset, 2).value_or(0xffff);
            key = 0x40000000u | scene;
            place = fmt::format("scene {:#04x}", scene);
        }
    }
    if (key == last_key) return;
    last_key = key;
    Common::HitchTrace::SetPlace(std::move(place));
}
} // namespace MM3D::HitchPlace
