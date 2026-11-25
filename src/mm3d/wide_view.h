// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Immersive wide view (visionOS prototype): the game renders a wider field of
// view than the 3DS, and every presented frame carries the game camera's
// orientation and the angular size of its image. The frontend shows the frame
// on a world-locked curved surface, so head movement between game frames
// needs no new frame.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

namespace MM3D::WideView {

inline std::atomic<bool> enabled{false};
/// The vertical field of view a 45-degree gameplay camera gets (the 3DS
/// camera's FOV is vertical). Other cameras widen in proportion.
inline std::atomic<float> fov{100.f};
constexpr float MinFov = 50.f, MaxFov = 120.f;
/// Gyroscope scale for motion controls while the immersive view is on.
constexpr float motion_sensitivity = 0.5f;

/// The HUD in the wide view covers about the angle it covers on a 3DS (a
/// 45-degree vertical view), times this margin, centred on the view.
constexpr float hud_spread = 1.2f;
/// Clip-space scale for HUD draws given the world image's vertical tangent.
inline float HudScale(float tan_y) {
    constexpr float native_tan_y = 0.41421356f; // tan(22.5 degrees)
    return tan_y > native_tan_y ? std::min(1.f, hud_spread * native_tan_y / tan_y) : 1.f;
}

/// The HUD's own layer: HUD draws that blend plainly (source alpha) skip the
/// game image and are replayed into this private layer, supersampled toward
/// this many pixels across the screen's short side, for a separate plane.
inline constexpr const char* HudLayerId = "@wide_hud";
constexpr float HudLayerTarget = 1440.f;
inline float HudLayerScale(int short_side) {
    return short_side > 0 ? std::clamp(HudLayerTarget / float(short_side), 1.f, 2.f) : 1.f;
}

/// The game camera is in a first-person mode, where gyro aiming turns it.
inline std::atomic<bool> first_person{false};

/// A full-screen menu or a framed close-up is up (the pause pages, the Song
/// of Soaring map, the Bombers' Notebook, playing the ocarina, a mask
/// transformation). The game then draws as on a 3DS: no head look, no
/// widening, no separate HUD, and the presenter shows its screen on a flat
/// panel. CPU thread writes.
inline std::atomic<bool> menu{false};
/// The game's screen context (native GetScreenContext); +8 is the current
/// screen. Gameplay is 0x639e44. Menu pages as MenuPageSwitch's (Items,
/// Masks, Gear, Map, which the Soaring map also uses); 0x63a3bc the Bombers'
/// Notebook, 0x63a2d0 the ocarina and 0x63a5b0 a mask transformation, from
/// the immersive view's screen log.
constexpr std::uint32_t ScreenContext = 0x72f5bc;
constexpr std::array<std::uint32_t, 7> MenuPageVtables{0x639ec4, 0x639f74, 0x63a270, 0x639de4,
                                                       0x63a3bc, 0x63a2d0, 0x63a5b0};
/// The current screen's vtable, or 0. CPU thread.
template <class M>
std::uint32_t CurrentScreen(M& m) {
    const auto current = m.Read(ScreenContext + 8);
    if (!current || *current < 0x100000 || (*current & 3) || *current >= 0x40000000) return 0;
    return m.Read(*current).value_or(0);
}
inline bool MenuScreen(std::uint32_t vtable) {
    return std::find(MenuPageVtables.begin(), MenuPageVtables.end(), vtable) != MenuPageVtables.end();
}

/// Head look (radians): yaw is the head's turn since the presenter's recentre,
/// positive to the left about world up; pitch is the head's raise since the
/// recentre, up positive; anchor_pitch is the head's elevation at the
/// recentre. The game world is held in the room by one rotation that tilts
/// it toward putting the camera's own view (the character) where the head
/// pointed at the recentre (`centring`), and turning the head looks around
/// that world without twisting it. Outside first person, RotateView turns the rendered view
/// in place, around the camera's eye; the game's own camera never sees it.
inline std::atomic<bool> head_look{false};
inline std::atomic<float> head_yaw{0.f}, head_pitch{0.f}, anchor_pitch{0.f};
/// How far the game world tilts to keep the camera's own view (the
/// character) at the recentre height, 0-1: 0 keeps the game's horizon on the
/// room's (the character sits where the camera looks down to), 1 keeps the
/// character centred (the horizon tilts with the camera's pitch, as on a
/// 3DS). The tilt follows the camera's pitch over about TiltSeconds, so a
/// jump moves the character, not the world; a cut snaps it.
inline std::atomic<float> centring{0.5f};
constexpr float TiltSeconds = 1.5f;
constexpr float TiltCut = 0.35f;
/// CPU thread: the camera elevation the tilt follows, eased, and when.
inline std::optional<float> tilt_elevation;
inline std::chrono::steady_clock::time_point tilt_time;

/// The turn RotateView applied to the newest view (CPU thread to renderer).
inline std::atomic<bool> applied_look{false};
inline std::atomic<float> applied_yaw{0.f}, applied_pitch{0.f};

/// The turns RotateView applied recently, by the view direction they gave.
/// The game builds a frame's view while the GPU still draws the previous one,
/// so the renderer finds the turn behind each drawn frame by its direction.
struct AppliedLook {
    float forward[3];
    float yaw, pitch;
    /// The view's direction before the turn.
    float base[3];
};
inline std::mutex applied_mutex;
/// The game builds its view more than once a frame (some areas more often),
/// and the renderer can be frames behind: keep plenty, without repeats.
inline std::array<AppliedLook, 64> applied_looks{};
inline unsigned applied_count = 0;

/// Renderer thread: the turn behind a drawn view, by its direction
/// (`forward`: the drawn camera's, world space, unit length). The game also
/// builds some views without the turn: a direction nearest an unturned one
/// gives no turn (yaw 0 and the base's own pitch). The newest, nearest
/// candidate wins; `exact` says whether it matched within 0.6 degrees.
struct LookMatch {
    AppliedLook look;
    bool unturned = false;
    bool exact = false;
    float dot = -2.f;
};
inline std::optional<LookMatch> MatchAppliedLook(const float forward[3]) {
    std::scoped_lock lock{applied_mutex};
    const unsigned available = std::min<unsigned>(applied_count, applied_looks.size());
    std::optional<LookMatch> best;
    const auto dot = [&](const float v[3]) {
        return v[0] * forward[0] + v[1] * forward[1] + v[2] * forward[2];
    };
    for (unsigned i = 0; i < available; ++i) {
        const auto& look = applied_looks[(applied_count - 1 - i) % applied_looks.size()];
        for (const bool unturned : {false, true}) {
            const float d = dot(unturned ? look.base : look.forward);
            if (!best || d > best->dot + 1e-6f) {
                best = LookMatch{look, unturned, d > 0.99995f, d};
            }
        }
    }
    if (best && best->unturned) {
        best->look.yaw = 0.f;
        best->look.pitch = 0.f;
    }
    return best;
}

/// Right after the game's LookAt has read the turned target, the original
/// goes back, so the game's own camera logic never sees the turn (it would
/// otherwise build on it frame after frame). The hook replaces the
/// instruction after LookAt's call, `add r1, r4, #380`.
constexpr std::uint32_t RestoreSVC = 0x00ffffe9;
/// Where the hook goes (USA, Restoration): the return address of LookAt's call.
constexpr std::array<std::uint32_t, 2> RestorePCs{0x18d4d0, 0x18d62c};
inline std::uint32_t RestoreInstruction(std::uint32_t pc, std::uint32_t word) {
    return (pc == RestorePCs[0] || pc == RestorePCs[1]) && word == 0xe2841f5f
               ? 0xef000000U | RestoreSVC
               : word;
}

/// Other places the game rebuilds the live view's LookAt (Restoration v110):
/// a lighter function that refreshes only the viewing matrix (0x16fce0) and
/// two callers at 0x5991b8 and 0x599870. Each is hooked on the instruction
/// before its LookAt call (turn the target) and the one after (put it back),
/// both `add` instructions (or a `nop`) the hook emulates. `block` is where the
/// translated (AOT) block holding the hook starts, so it can be sent to the
/// fallback CPU.
///
/// Both view functions (0x16fce0 and View_ApplyPerspective's 0x18d528) also
/// build a second viewing matrix (+428) from a second eye/at/up (+50, +5c,
/// +68). Left unturned, frames the game drew with it showed the unturned view
/// (about 1 frame in 90), which the immersive screen placed at the recentre
/// direction: a one-frame flash of the room. `second` sites turn that set.
struct LookAtSite {
    std::uint32_t pc, word;
    bool turn;
    std::uint32_t block;
    bool second = false;
};
constexpr std::array<LookAtSite, 10> LookAtSites{{
    {0x16fcf4, 0xe2800f5f, true, 0x16fce0},  {0x16fcfc, 0xe284305c, false, 0x16fcfc},
    {0x5991b4, 0xe28a0f5f, true, 0x5991a8},  {0x5991bc, 0xe28a1f5f, false, 0x5991bc},
    {0x59986c, 0xe2880fdd, true, 0x59985c},  {0x599874, 0xe2851f5f, false, 0x599874},
    {0x16fd08, 0xe2840f6b, true, 0x16fcfc, true}, {0x16fd10, 0xe2841f5f, false, 0x16fd10, true},
    {0x18d660, 0xe2840f6b, true, 0x18d654, true}, {0x18d66c, 0xe320f000, false, 0x18d668, true},
}};
constexpr std::uint32_t LookAtSVC = 0x00ffff40; // + site index
inline std::uint32_t LookAtInstruction(std::uint32_t pc, std::uint32_t word) {
    for (std::uint32_t i = 0; i < LookAtSites.size(); ++i) {
        if (pc == LookAtSites[i].pc && word == LookAtSites[i].word) {
            return 0xef000000U | (LookAtSVC + i);
        }
    }
    return word;
}
/// Emulates a site's `add rd, rn, #imm` (or `nop`) on `regs`.
template <class Regs>
void EmulateLookAtAdd(const LookAtSite& site, Regs& regs) {
    if (site.word == 0xe320f000) return;
    const std::uint32_t rd = (site.word >> 12) & 15, rn = (site.word >> 16) & 15;
    const std::uint32_t rotate = ((site.word >> 8) & 15) * 2;
    const std::uint32_t value = site.word & 0xff;
    const std::uint32_t imm = rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
    regs[rd] = regs[rn] + imm;
}

/// Whether `pc` is where the turned target is still needed: in the view
/// function (USA, Restoration) up to its RestoreSVC hook, between another
/// LookAt site's hooks, or in LookAt itself and its helper.
inline bool InViewFunction(std::uint32_t pc) {
    if ((pc >= 0x18d3cc && pc <= 0x18d4d0) || (pc >= 0x18d528 && pc <= 0x18d62c) ||
        (pc >= 0x1b6498 && pc < 0x1b66f4)) {
        return true;
    }
    for (std::size_t i = 0; i + 1 < LookAtSites.size(); i += 2) {
        if (pc >= LookAtSites[i].pc && pc <= LookAtSites[i + 1].pc) return true;
    }
    return false;
}

/// CPU thread: what RotateView last wrote, to recognise a view the game did
/// not rebuild (and so must not be turned twice).
struct ViewRotation {
    bool valid = false;
    std::uint32_t view = 0;
    std::array<std::uint32_t, 3> eye{}, written{}, original{};
    /// The up vector (+40), turned along with the target.
    std::array<std::uint32_t, 3> up_written{}, up_original{};
    /// The emulated core that ran the view function.
    unsigned core = 0;
};

inline ViewRotation view_rotation;
/// The same for the second eye/at/up (+50, see LookAtSites), turned as a view
/// at +28 so the offsets match.
inline ViewRotation second_view_rotation;

/// Called where the game builds the live view's matrices (the FOV hook in
/// View_ApplyPerspective, before its LookAt reads eye +28, at +34, up +40):
/// turns `at` around `eye` by the head look.
template <class M>
void RotateView(M& m, std::uint32_t view, ViewRotation& state = view_rotation) {
    std::array<std::uint32_t, 3> eye{}, at{}, up{};
    for (unsigned i = 0; i < 3; ++i) {
        const auto e = m.Read(view + 0x28 + 4 * i), a = m.Read(view + 0x34 + 4 * i),
                   u = m.Read(view + 0x40 + 4 * i);
        if (!e || !a || !u) return;
        eye[i] = *e;
        at[i] = *a;
        up[i] = *u;
    }
    // A second set copied from the live view while it was turned is turned already.
    if (&state != &view_rotation && view_rotation.eye == eye && view_rotation.written == at) {
        return;
    }
    auto original = at;
    auto up_original = up;
    // The game kept the view we turned: start again from its own target and up.
    if (state.valid && state.view == view && state.written == at && state.eye == eye &&
        state.up_written == up) {
        original = state.original;
        up_original = state.up_original;
    }
    const bool active = enabled.load() && head_look.load() && !first_person.load() && !menu.load();
    const auto write = [&](std::uint32_t offset, const std::array<std::uint32_t, 3>& value) {
        for (unsigned i = 0; i < 3; ++i) m.Write(view + offset + 4 * i, value[i]);
    };
    if (!active) {
        if (&state == &view_rotation) tilt_elevation.reset();
        if (original != at) write(0x34, original);
        if (up_original != up) write(0x40, up_original);
        state.valid = false;
        applied_look.store(false);
        return;
    }
    float f[3];
    for (unsigned i = 0; i < 3; ++i) {
        f[i] = std::bit_cast<float>(original[i]) - std::bit_cast<float>(eye[i]);
        if (!std::isfinite(f[i])) return;
    }
    const float length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const float level = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (length < 1e-3f || level < 1e-4f) return;
    const float yaw = head_yaw.load();
    const float anchor = anchor_pitch.load();
    const float head = std::clamp(anchor + head_pitch.load(), -1.4f, 1.4f);
    // The camera's own heading and elevation (right-handed, y up, as the
    // game's LookAt; heading 0 looks down -z).
    const float base_yaw = std::atan2(-f[0], -f[2]);
    const float elevation = std::atan2(f[1], level);
    // The primary view eases the elevation the tilt follows.
    if (&state == &view_rotation) {
        const auto now = std::chrono::steady_clock::now();
        if (!tilt_elevation || std::abs(elevation - *tilt_elevation) > TiltCut) {
            tilt_elevation = elevation;
        } else {
            const float dt = std::clamp(std::chrono::duration<float>(now - tilt_time).count(),
                                        0.f, 0.1f);
            *tilt_elevation += (elevation - *tilt_elevation) * (1.f - std::exp(-dt / TiltSeconds));
        }
        tilt_time = now;
    }
    const float tilt = std::clamp(centring.load(), 0.f, 1.f) *
                       (tilt_elevation.value_or(elevation) - anchor);
    // Camera to game world: the room's head pose (turn, then elevation)
    // carried into the game by the world's tilt about the recentre heading:
    // Yaw(base) * Pitch(tilt) * Yaw(turn) * Pitch(head). Full centring
    // (tilt = elevation - anchor) takes the recentre direction to the
    // camera's own view.
    using V = std::array<float, 3>;
    const auto rotate_y = [](float a, const V& v) {
        const float c = std::cos(a), s = std::sin(a);
        return V{v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c};
    };
    const auto rotate_x = [](float a, const V& v) {
        const float c = std::cos(a), s = std::sin(a);
        return V{v[0], v[1] * c - v[2] * s, v[1] * s + v[2] * c};
    };
    const auto to_game = [&](const V& v) {
        return rotate_y(base_yaw, rotate_x(tilt, rotate_y(yaw, rotate_x(head, v))));
    };
    const V new_dir = to_game(V{0.f, 0.f, -1.f});
    const V new_level = to_game(V{0.f, 1.f, 0.f});
    const float turned[3] = {new_dir[0] * length, new_dir[1] * length, new_dir[2] * length};
    std::array<std::uint32_t, 3> result{};
    for (unsigned i = 0; i < 3; ++i) {
        result[i] = std::bit_cast<std::uint32_t>(std::bit_cast<float>(eye[i]) + turned[i]);
    }
    // The camera's up is square to its own direction (it leans back as the
    // camera looks down). Left as it was, turning the target would roll the
    // view. Rebuild it for the new direction with the camera's original roll
    // about its direction (normally none).
    const auto dot = [](const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto cross = [](const V& a, const V& b) {
        return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto normalize = [&](V v) {
        const float l = std::sqrt(dot(v, v));
        return l > 1e-6f ? V{v[0] / l, v[1] / l, v[2] / l} : V{0.f, 0.f, 0.f};
    };
    // The up square to `dir` with no roll: world up with its `dir` part removed.
    const auto level_up = [&](const V& dir) {
        const float along = dir[1];
        return normalize(V{-along * dir[0], 1.f - along * dir[1], -along * dir[2]});
    };
    const V old_dir{f[0] / length, f[1] / length, f[2] / length};
    V up_in{};
    for (unsigned i = 0; i < 3; ++i) up_in[i] = std::bit_cast<float>(up_original[i]);
    const float up_along = dot(up_in, old_dir);
    const V up_square = normalize(V{up_in[0] - up_along * old_dir[0], up_in[1] - up_along * old_dir[1],
                                    up_in[2] - up_along * old_dir[2]});
    const V old_level = level_up(old_dir);
    float roll_cos = 1.f, roll_sin = 0.f;
    if (dot(up_square, up_square) > 0.5f && dot(old_level, old_level) > 0.5f) {
        roll_cos = dot(old_level, up_square);
        roll_sin = dot(cross(old_level, up_square), old_dir);
    }
    // The view's up in the game: rolled where the head turns away from the
    // recentre, as the tilted world is; then the camera's own roll.
    const V side = cross(new_dir, new_level);
    std::array<std::uint32_t, 3> up_result = up_original;
    if (dot(new_level, new_level) > 0.5f) {
        for (unsigned i = 0; i < 3; ++i) {
            up_result[i] = std::bit_cast<std::uint32_t>(new_level[i] * roll_cos + side[i] * roll_sin);
        }
    }
    write(0x34, result);
    write(0x40, up_result);
    state.valid = true;
    state.view = view;
    state.eye = eye;
    state.written = result;
    state.original = original;
    state.up_written = up_result;
    state.up_original = up_original;
    {
        AppliedLook look{{turned[0] / length, turned[1] / length, turned[2] / length}, yaw,
                         head - anchor, {f[0] / length, f[1] / length, f[2] / length}};
        std::scoped_lock lock{applied_mutex};
        const bool repeat = applied_count > 0 && [&] {
            const auto& newest = applied_looks[(applied_count - 1) % applied_looks.size()];
            return newest.yaw == look.yaw && newest.pitch == look.pitch &&
                   newest.forward[0] == look.forward[0] && newest.forward[1] == look.forward[1] &&
                   newest.forward[2] == look.forward[2];
        }();
        if (!repeat) applied_looks[applied_count++ % applied_looks.size()] = look;
    }
    applied_yaw.store(yaw);
    applied_pitch.store(head - anchor);
    applied_look.store(true);
}

/// Reads the active Play camera's mode (Restoration's camera array, as in
/// CameraLive::ScriptedCamera): SUBJECT (6), PACHINCO (12) and BOWARROW (13)
/// are first person. CPU thread.
template <class M>
std::optional<bool> FirstPersonCamera(M& m) {
    const auto pointer = [](std::uint32_t p, std::uint32_t size) {
        return p >= 0x100000 && !(p & 3) && p < 0x40000000 && size <= 0x40000000 - p;
    };
    const auto context = m.Read(0x72f5c0);
    if (!context || !pointer(*context, 0x170)) return {};
    const auto game = m.Read(*context + 0x16c);
    if (!game || !pointer(*game, 0xa7c)) return {};
    const auto indices = m.Read(*game + 0xa78);
    if (!indices) return {};
    const unsigned index = *indices >> 16;
    if (index > 3) return {};
    const std::uint32_t camera = *game + 0x408 + index * 0x198;
    const auto owner = m.Read(camera + 0xa0), mode = m.Read(camera + 0x158);
    if (owner != game || !mode) return {};
    const unsigned value = *mode & 0xffff;
    return value == 6 || value == 12 || value == 13;
}

struct Camera {
    bool valid = false;
    /// Camera-to-world rotation as a quaternion (x, y, z, w) in the game's
    /// world (y up). The camera looks down -z with y up and x right.
    float rotation[4]{0.f, 0.f, 0.f, 1.f};
    /// Tangents of half the presented image's horizontal and vertical angle.
    float tan_x = 0.f, tan_y = 0.f;
    /// The head look this frame's view was turned by (RotateView), if any.
    bool head_look = false;
    float head_yaw = 0.f, head_pitch = 0.f;
    /// The game built this view without the head look while the head was
    /// turned (MatchAppliedLook's unturned case). Placed where it was rendered,
    /// it would flash away from where the player looks, so the presenter skips it.
    bool unturned = false;
    /// A full-screen menu is open (`menu`): the image holds the whole screen
    /// as on a 3DS, without the HUD layer, for the presenter to show flat.
    bool no_world = false;
    /// The HUD layer for this frame (id<MTLTexture>, valid during delivery),
    /// in the guest framebuffer's rotated orientation: its x runs up the
    /// screen from the bottom edge, its y leftward from the right edge.
    /// nullptr if none.
    void* hud_texture = nullptr;
    std::uint32_t hud_width = 0, hud_height = 0;
    /// UI added by the host compositor (replacement minimap and Game Options),
    /// in final display orientation. It is kept separate so it does not bend
    /// with the world image.
    void* host_hud_texture = nullptr;
};

/// `camera` is the verified world shader's camera matrix (uniforms f70-72,
/// camera to world) and `projection` its projection (f0-3), both row-major
/// 4x4. `zoom_out` and `horizontal_scale` are the host clip-space scales the
/// rasterizer applies to top-screen world draws (1 and 1 when unused).
template <class Matrix>
Camera FromMatrices(const Matrix& camera, const Matrix& projection, float zoom_out,
                    float horizontal_scale) {
    Camera result;
    // The top screen is rendered rotated: camera x and y each feed one clip axis.
    const double scale_x = std::max(std::abs(projection[0][0]), std::abs(projection[1][0]));
    const double scale_y = std::max(std::abs(projection[0][1]), std::abs(projection[1][1]));
    if (!(scale_x > 1e-4) || !(scale_y > 1e-4) || !(horizontal_scale > 0.f)) {
        return result;
    }
    result.tan_x = float(zoom_out / scale_x / horizontal_scale);
    result.tan_y = float(zoom_out / scale_y);
    const auto m = [&](int r, int c) { return double(camera[r][c]); };
    double x, y, z, w;
    const double trace = m(0, 0) + m(1, 1) + m(2, 2);
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2;
        w = s / 4;
        x = (m(2, 1) - m(1, 2)) / s;
        y = (m(0, 2) - m(2, 0)) / s;
        z = (m(1, 0) - m(0, 1)) / s;
    } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
        const double s = std::sqrt(1.0 + m(0, 0) - m(1, 1) - m(2, 2)) * 2;
        w = (m(2, 1) - m(1, 2)) / s;
        x = s / 4;
        y = (m(0, 1) + m(1, 0)) / s;
        z = (m(0, 2) + m(2, 0)) / s;
    } else if (m(1, 1) > m(2, 2)) {
        const double s = std::sqrt(1.0 + m(1, 1) - m(0, 0) - m(2, 2)) * 2;
        w = (m(0, 2) - m(2, 0)) / s;
        x = (m(0, 1) + m(1, 0)) / s;
        y = s / 4;
        z = (m(1, 2) + m(2, 1)) / s;
    } else {
        const double s = std::sqrt(1.0 + m(2, 2) - m(0, 0) - m(1, 1)) * 2;
        w = (m(1, 0) - m(0, 1)) / s;
        x = (m(0, 2) + m(2, 0)) / s;
        y = (m(1, 2) + m(2, 1)) / s;
        z = s / 4;
    }
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(length) || length < 1e-6) {
        return result;
    }
    result.rotation[0] = float(x / length);
    result.rotation[1] = float(y / length);
    result.rotation[2] = float(z / length);
    result.rotation[3] = float(w / length);
    result.valid = true;
    return result;
}

/// The RestoreSVC hook: puts back the target RotateView replaced. Also called
/// at the start of every CPU run slice on the core that turned the view,
/// outside the view function, for translated (AOT) code made before the hook.
template <class M>
void RestoreView(M& m, std::uint32_t view, ViewRotation& state = view_rotation) {
    if (!state.valid || state.view != view) return;
    for (unsigned i = 0; i < 3; ++i) {
        if (m.Read(view + 0x34 + 4 * i) != std::optional<std::uint32_t>(state.written[i])) return;
    }
    for (unsigned i = 0; i < 3; ++i) m.Write(view + 0x34 + 4 * i, state.original[i]);
    for (unsigned i = 0; i < 3; ++i) {
        if (m.Read(view + 0x40 + 4 * i) == std::optional<std::uint32_t>(state.up_written[i])) {
            m.Write(view + 0x40 + 4 * i, state.up_original[i]);
        }
    }
    state.valid = false;
}

/// Renderer thread only: the world camera of the newest two guest frames,
/// and where the game camera's own centre (before the head look turned the
/// view) lands in the turned view, in clip units, for the HUD.
struct Capture {
    std::uint64_t guest_frame = ~std::uint64_t{};
    Camera previous, current;
    /// HudTransform of the newest frame, without the HUD's own scale.
    std::array<double, 9> hud{1, 0, 0, 0, 1, 0, 0, 0, 1};
};

/// `projection` as for FromMatrices, of a view RotateView turned by `yaw` and
/// `pitch` from a recentre at `anchor` (its head_yaw, head_pitch and
/// anchor_pitch); `scale_x` and `scale_y` the host clip scales of the world
/// draws. Returns the 3x3 projective transform (row major) that takes a point
/// of the unturned view's image (clip x, y, 1) to where that direction is in
/// the turned view (final clip x, y, w): the HUD keeps the unturned view's
/// place and shape, seen from the turned one. The turn in the camera's own
/// axes is Pitch(-(anchor + pitch)) * Yaw(-yaw) * Pitch(anchor), whatever the
/// camera's direction (the game's cameras do not roll).
template <class Matrix>
std::optional<std::array<double, 9>> HudTransform(const Matrix& projection, float yaw,
                                                  float pitch, float anchor, float scale_x,
                                                  float scale_y) {
    using M3 = std::array<std::array<double, 3>, 3>;
    const auto about_x = [](double a) {
        const double c = std::cos(a), s = std::sin(a);
        return M3{{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
    };
    const auto about_y = [](double a) {
        const double c = std::cos(a), s = std::sin(a);
        return M3{{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
    };
    const auto times = [](const M3& a, const M3& b) {
        M3 out{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        return out;
    };
    const double head = std::clamp(double(anchor) + pitch, -1.4, 1.4);
    // Unturned camera coordinates to turned.
    const M3 turn = times(times(about_x(-head), about_y(-yaw)), about_x(anchor));
    double rel[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rel[i][j] = turn[i][j];
    // Clip point -> direction (camera z = -1), through the projection's x/y block.
    const double m00 = projection[0][0], m01 = projection[0][1];
    const double m10 = projection[1][0], m11 = projection[1][1];
    const double det = m00 * m11 - m01 * m10;
    if (std::abs(det) < 1e-9) return {};
    const double inv[2][2] = {{m11 / det, -m01 / det}, {-m10 / det, m00 / det}};
    const double p2[2] = {double(projection[0][2]), double(projection[1][2])};
    const double unproject[3][3] = {
        {inv[0][0], inv[0][1], inv[0][0] * p2[0] + inv[0][1] * p2[1]},
        {inv[1][0], inv[1][1], inv[1][0] * p2[0] + inv[1][1] * p2[1]},
        {0, 0, -1}};
    const double project[3][3] = {{m00, m01, double(projection[0][2])},
                                  {m10, m11, double(projection[1][2])},
                                  {0, 0, -1}};
    const auto multiply = [](const double a[3][3], const double b[3][3], double out[3][3]) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    };
    double turned[3][3], full[3][3];
    multiply(rel, unproject, turned);
    multiply(project, turned, full);
    // Into the final clip space: scale rows by the world draws' clip scales.
    const double scale[3] = {scale_x, scale_y, 1};
    std::array<double, 9> result{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) result[i * 3 + j] = full[i][j] * scale[i];
    return result;
}
inline Capture capture;

/// The first verified world camera of each guest frame.
inline void Record(std::uint64_t guest_frame, const Camera& camera) {
    if (guest_frame == capture.guest_frame || !camera.valid) {
        return;
    }
    capture.previous = capture.current;
    capture.current = camera;
    capture.guest_frame = guest_frame;
}

/// The camera of the frame being presented: halfway between the last two
/// guest frames for an interpolated (60 FPS) frame.
inline Camera Presented(bool midpoint) {
    const Camera& a = capture.previous;
    const Camera& b = capture.current;
    if (!midpoint || !a.valid || !b.valid) {
        return b;
    }
    Camera result = b;
    // Halfway to an unturned view is no truer a placement.
    result.unturned = a.unturned || b.unturned;
    float dot = 0;
    for (int i = 0; i < 4; ++i) {
        dot += a.rotation[i] * b.rotation[i];
    }
    const float sign = dot < 0 ? -1.f : 1.f;
    float length = 0;
    for (int i = 0; i < 4; ++i) {
        result.rotation[i] = a.rotation[i] * sign + b.rotation[i];
        length += result.rotation[i] * result.rotation[i];
    }
    length = std::sqrt(length);
    if (length < 1e-6f) {
        return b;
    }
    for (float& value : result.rotation) {
        value /= length;
    }
    result.tan_x = (a.tan_x + b.tan_x) / 2;
    result.tan_y = (a.tan_y + b.tan_y) / 2;
    if (a.head_look && b.head_look) {
        result.head_yaw = a.head_yaw + std::remainder(b.head_yaw - a.head_yaw, 6.2831853f) / 2;
        result.head_pitch = (a.head_pitch + b.head_pitch) / 2;
    }
    return result;
}

} // namespace MM3D::WideView
