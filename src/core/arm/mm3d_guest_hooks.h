// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#ifdef ENABLE_MM3D_PRESENTATION

#include <array>
#include <optional>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "mm3d/camera_writer.h"
#include "mm3d/classic_double_time.h"
#include "mm3d/notebook_inspector.h"
#include "mm3d/camera_instruction_probe.h"
#include "mm3d/form_camera.h"
#include "mm3d/camera_live.h"
#include "mm3d/camera_midpoint.h"
#include "mm3d/link_pose_capture.h"
#include "mm3d/diagnostics.h"
#include "mm3d/minimap.h"
#include "mm3d/native_minimap.h"
#include "mm3d/minimap_shortcut.h"
#include "mm3d/native_inventory.h"
#include "mm3d/dungeon_map.h"
#include "mm3d/difficulty.h"
#include "mm3d/actor_draw_distance.h"
#include "mm3d/prop_draw_distance.h"
#include "mm3d/vegetation_draw_distance.h"
#include "mm3d/death_penalties.h"
#include "mm3d/classic_save_hooks.h"
#include "mm3d/bottle_healing.h"
#include "mm3d/inventory_shadow.h"
#include "mm3d/items_map_prompt.h"
#include "mm3d/menu_help.h"
#include "mm3d/world_map_prompt.h"
#include "mm3d/menu_tabs.h"
#include "mm3d/pause_capture.h"
#include "mm3d/visibility_depth.h"
#include "mm3d/native_notebook.h"
#include "mm3d/native_file_select.h"
#include "mm3d/file_select_prompt.h"
#include "mm3d/ocarina_prompt.h"
#include "mm3d/magic_border.h"
#include "mm3d/minigame_hud.h"
#include "mm3d/player_hud.h"
#include "mm3d/song_list.h"
#include "mm3d/double_time.h"
#include "mm3d/song_reset.h"
#include "mm3d/hud_buttons.h"
#include "mm3d/banners.h"
#include "mm3d/song_effect.h"
#include "mm3d/native_item_present.h"
#include "mm3d/inventory_navigation.h"
#include "mm3d/menu_page_switch.h"
#include "mm3d/menu_memory.h"
#include "mm3d/menu_backdrop.h"
#include "mm3d/screen_shrink.h"
#include "mm3d/single_screen_text.h"
#include "mm3d/prices.h"
#include "mm3d/menu_sound.h"
#include "mm3d/music.h"
#include "mm3d/tatl_reminders.h"
#include "mm3d/owl_sword.h"
#include "mm3d/owl_intro.h"
#include "mm3d/bomber_approach.h"
#include "mm3d/menu_close.h"
#include "mm3d/menu_shortcuts.h"
#include "mm3d/options_menu.h"
#include "mm3d/native_startup.h"
#include "mm3d/item_preview.h"
#include "mm3d/dialogue_scale.h"
#include "mm3d/classic_owl_draw.h"
#include "mm3d/classic_file_marker.h"
#include "mm3d/widescreen.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"

namespace Core {

/// MM3D enhancements hooked into guest execution: code substitutions applied
/// at instruction fetch, the SVCs those substitutions call, camera-write
/// tracing, and per-slice services. Shared by every CPU backend so the
/// enhancements behave the same with Dynarmic and with the DynCom interpreter.
class MM3DGuestHooks {
public:
    /// Backend-specific access to the guest CPU state.
    class Cpu {
    public:
        virtual ~Cpu() = default;
        virtual std::array<u32, 16>& Regs() = 0;
        virtual std::array<u32, 64>& ExtRegs() = 0;
        virtual u32 Cpsr() const = 0;
        virtual void SetCpsr(u32 value) = 0;
        virtual bool CameraTracing() const = 0;
        virtual u32 CoreId() const = 0;
    };

    MM3DGuestHooks(Core::System& system, Memory::MemorySystem& memory, Cpu& cpu)
        : system(system), memory(memory), cpu(cpu) {}

    /// Reads the instruction at `vaddr`, substituting any enabled MM3D patch.
    std::optional<u32> ReadCode(VAddr vaddr);

    /// Handles an MM3D hook SVC. Must be called with R15 holding the address of
    /// the instruction after the SVC. Returns false for ordinary kernel SVCs.
    bool CallSVC(u32 swi);

    /// Guest 32-bit read with interpolation diagnostics attribution.
    u32 Read32(VAddr vaddr);

    /// Services that run around each slice of guest execution.
    void BeforeRun(Memory::PageTable* page_table);
    void AfterRun(Memory::PageTable* page_table);

    /// Drops menu hook state tied to translated code.
    void ResetMenuState() {
        menu_page_switch = {};
        menu_memory = {};
        menu_backdrop = {};
        menu_sound_context = 0;
    }

    MM3D::FormCamera::Hook form_camera;
    MM3D::FormCamera::ArrivalLens arrival_lens;
    MM3D::FormCamera::OcarinaReturnLens ocarina_return_lens;
    MM3D::FormCamera::DekuFreeCamera deku_free_camera;
    MM3D::FormCamera::EntryRecoveryOrbit entry_recovery_orbit;
    unsigned form_trace_count=0,form_projection_count=0;
    float form_last_native=-1.f,form_last_output=-1.f,form_last_radius=-1.f;
    u32 form_last_mode=~0u;
    std::array<u32,6> form_last_pose{};
    bool form_trace_was_active=false;
    MM3D::NativeMinimap::Hook minimap_hook;
    MM3D::DungeonMap::Hook dungeon_map_hook;
    MM3D::InventoryShadow::Hook inventory_shadow_hook;
    MM3D::ItemsMapPrompt::Hook items_map_prompt_hook,map_items_prompt_hook;
    MM3D::MenuHelp::Hook menu_help_hook,select_help_hook;
    MM3D::WorldMapPrompt::Hook world_map_prompt_hook;
    MM3D::MenuTabs::Hook menu_tabs_hook;
    MM3D::NativeInventory::Hook inventory_hook;
    MM3D::OptionsMenu::Hook options_hook;
    MM3D::MenuPageSwitch::Hook menu_page_switch;
    MM3D::MenuMemory::Hook menu_memory;
    MM3D::MenuBackdrop::Hook menu_backdrop;
    u32 menu_sound_context=0;
    MM3D::PauseCapture::Hook pause_capture_hook;
    MM3D::VisibilityDepth::Hook visibility_depth_hook;
    MM3D::NativeNotebook::Hook notebook_hook;
    MM3D::NativeFileSelect::Hook file_select_hook;
    MM3D::FileSelectPrompt::Hook file_select_prompt_hook;
    MM3D::OcarinaPrompt::Hook ocarina_hook;
    MM3D::MagicBorder::Hook magic_hook;
    MM3D::MinigameHud::Hook minigame_hook;
    MM3D::PlayerHud::Hook player_hud_hook;
    MM3D::SongList::Hook song_hook;
    MM3D::DoubleTime::Hook double_time_hook;
    MM3D::ClassicDoubleTime::Hook classic_double_time_hook;
    MM3D::SongReset::Hook song_reset_hook;
    MM3D::HudButtons::Hook hud_buttons_hook;
    MM3D::Banners::Hook banner_hook;
    MM3D::NativeItemPresent::Hook item_present_hook;
    MM3D::NativeStartup::Hook startup_hook;
    MM3D::LinkPoseCapture::Hook link_pose_hook;
    MM3D::NativeStartup::PressHook press_hook;
    MM3D::NativeStartup::LoadingIconHook loading_icon_hook;
    MM3D::ItemPreview::Hook item_preview_hook;
    MM3D::ClassicSaving::Hook classic_save_hook;
    MM3D::ClassicSaving::OwlDraw classic_owl_draw;
    MM3D::ClassicSaving::FileMarker classic_file_marker;
    // Bounded observation for the unresolved sword-activation movement hold.
    u32 owl_trace_actor=0,owl_trace_play=0,owl_trace_ticks=0;
    void TraceOwlState(const char* stage) {
        if(!owl_trace_actor||!owl_trace_play)return;
        const u32 player=memory.Read32OrNullopt(owl_trace_play+0x20dc).value_or(0);
        const auto read=[&](u32 at){return memory.Read32OrNullopt(at).value_or(0xffffffff);};
        LOG_INFO(Core_ARM11,"Owl activation trace: stage={} tick={} owl={:08x} play={:08x} player={:08x} owl_state={:08x} owl_action={:08x} actor_flags={:08x} player_action={:08x} talk_actor={:08x} talk_state={:08x} player_flags={:08x}/{:08x} cs={:08x}/{:08x}/{:08x} message={:08x}",
            stage,owl_trace_ticks,owl_trace_actor,owl_trace_play,player,
            read(owl_trace_actor+0x264),read(owl_trace_actor+0x268),read(owl_trace_actor+4),
            player?read(player+0x92c):0,player?read(player+0x930):0,player?read(player+0x950):0,
            player?read(player+0x11db0):0,player?read(player+0x11db4):0,
            read(0x687c14),read(0x687c18),read(0x687c20),read(owl_trace_play+0x825c));
    }

    MM3D::DialogueScale::Hook dialogue_hook;
    MM3D::CameraWriter::Instruction camera_instruction;
    void TraceCameraWrite(VAddr address, std::uint32_t value) {
        if(!cpu.CameraTracing() || !MM3D::CameraWriter::active.load() ||
           !MM3D::CameraWriter::Matches(value)) return;
        MM3D::CameraWriter::Hit hit;
        hit.instruction = camera_instruction; hit.address = address; hit.value = value;
        hit.core = cpu.CoreId();
        const auto process = system.Kernel().GetCurrentProcess();
        if(!process) return;
        hit.process = process->process_id;
        hit.instruction_word = memory.Read32OrNullopt(hit.instruction.pc & ~3u).value_or(0);
        for(unsigned i=0; i<hit.stack.size(); ++i) {
            const std::uint64_t at = std::uint64_t(hit.instruction.sp) + i*4;
            if(at > 0xffffffffULL) break;
            const auto word = memory.Read32OrNullopt(static_cast<VAddr>(at));
            if(word) { hit.stack[i] = *word; hit.stack_valid |= 1u << i; }
        }
        MM3D::CameraWriter::Observe(hit);
    }

private:
    Core::System& system;
    Memory::MemorySystem& memory;
    Cpu& cpu;
};

} // namespace Core

#endif
