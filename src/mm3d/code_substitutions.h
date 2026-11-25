// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include "mm3d/actor_draw_distance.h"
#include "mm3d/banners.h"
#include "mm3d/bomber_approach.h"
#include "mm3d/bottle_healing.h"
#include "mm3d/camera_instruction_probe.h"
#include "mm3d/camera_live.h"
#include "mm3d/camera_midpoint.h"
#include "mm3d/camera_writer.h"
#include "mm3d/classic_double_time.h"
#include "mm3d/classic_file_marker.h"
#include "mm3d/classic_owl_draw.h"
#include "mm3d/classic_save_hooks.h"
#include "mm3d/death_penalties.h"
#include "mm3d/diagnostics.h"
#include "mm3d/dialogue_scale.h"
#include "mm3d/difficulty.h"
#include "mm3d/double_time.h"
#include "mm3d/dungeon_map.h"
#include "mm3d/file_select_prompt.h"
#include "mm3d/form_camera.h"
#include "mm3d/hud_buttons.h"
#include "mm3d/inventory_navigation.h"
#include "mm3d/inventory_shadow.h"
#include "mm3d/item_preview.h"
#include "mm3d/items_map_prompt.h"
#include "mm3d/link_pose_capture.h"
#include "mm3d/magic_border.h"
#include "mm3d/menu_backdrop.h"
#include "mm3d/menu_close.h"
#include "mm3d/menu_help.h"
#include "mm3d/menu_memory.h"
#include "mm3d/menu_page_switch.h"
#include "mm3d/menu_shortcuts.h"
#include "mm3d/menu_sound.h"
#include "mm3d/menu_tabs.h"
#include "mm3d/minigame_hud.h"
#include "mm3d/minimap.h"
#include "mm3d/minimap_shortcut.h"
#include "mm3d/music.h"
#include "mm3d/native_file_select.h"
#include "mm3d/native_inventory.h"
#include "mm3d/native_item_present.h"
#include "mm3d/native_minimap.h"
#include "mm3d/native_notebook.h"
#include "mm3d/native_startup.h"
#include "mm3d/notebook_inspector.h"
#include "mm3d/ocarina_prompt.h"
#include "mm3d/options_menu.h"
#include "mm3d/owl_intro.h"
#include "mm3d/owl_sword.h"
#include "mm3d/pause_capture.h"
#include "mm3d/player_hud.h"
#include "mm3d/prices.h"
#include "mm3d/prop_draw_distance.h"
#include "mm3d/screen_shrink.h"
#include "mm3d/single_screen_text.h"
#include "mm3d/song_effect.h"
#include "mm3d/song_list.h"
#include "mm3d/song_reset.h"
#include "mm3d/tatl_reminders.h"
#include "mm3d/vegetation_draw_distance.h"
#include "mm3d/visibility_depth.h"
#include "mm3d/widescreen.h"
#include "mm3d/world_map_prompt.h"

namespace MM3D {

/// MM3D's ARM process (USA).
constexpr std::uint64_t ProgramId = 0x0004000000125500ULL;

/// Applies the first MM3D code substitution that matches `word` at `vaddr`.
/// `code` reads original guest words: `std::optional<std::uint32_t> Read(std::uint32_t)`.
/// Returns `word` unchanged when nothing applies. The result is only valid
/// inside the MM3D process (ProgramId).
///
/// Shared by the CPU backends (MM3DGuestHooks::ReadCode) and the offline AOT
/// translator, so both see identical guest code.
template <class Code>
std::uint32_t SubstituteInstruction(Code& code, std::uint32_t vaddr, std::uint32_t word) {
    if (ScreenShrink::Instruction(vaddr, word) != word && ScreenShrink::CodeMatches(code))
        return ScreenShrink::Instruction(vaddr, word);
    if (ClassicSaving::Instruction(vaddr, word) != word && ClassicSaving::CodeMatches(code))
        return ClassicSaving::Instruction(vaddr, word);
    if (MinimapShortcut::Instruction(vaddr, word) != word && MinimapShortcut::CodeMatches(code))
        return MinimapShortcut::Instruction(vaddr, word);
    if (BottleHealing::Instruction(vaddr, word) != word && BottleHealing::CodeMatches(code))
        return BottleHealing::Instruction(vaddr, word);
    if (DeathPenalties::Instruction(vaddr, word) != word && DeathPenalties::CodeMatches(code))
        return DeathPenalties::Instruction(vaddr, word);
    if (Difficulty::SpawnInstruction(vaddr, word) != word && Difficulty::SpawnCodeMatches(code, vaddr))
        return Difficulty::SpawnInstruction(vaddr, word);
    if (PropDrawDistance::Instruction(vaddr, word) != word && PropDrawDistance::CodeMatches(code, vaddr))
        return PropDrawDistance::Instruction(vaddr, word);
    if (VegetationDrawDistance::Instruction(vaddr, word) != word && VegetationDrawDistance::CodeMatches(code, vaddr))
        return VegetationDrawDistance::Instruction(vaddr, word);
    if (ActorDrawDistance::Instruction(vaddr, word) != word && ActorDrawDistance::CodeMatches(code, vaddr))
        return ActorDrawDistance::Instruction(vaddr, word);
    if (Difficulty::Instruction(vaddr, word) != word && Difficulty::CodeMatches(code, vaddr))
        return Difficulty::Instruction(vaddr, word);
    if (NativeStartup::Instruction(vaddr, word) != word && NativeStartup::CodeMatches(code))
        return NativeStartup::Instruction(vaddr, word);
    if (ScheduleRows::Instruction(vaddr, word) != word && ScheduleRows::CodeMatches(code))
        return ScheduleRows::Instruction(vaddr, word);
    if (NotebookRows::Instruction(vaddr, word) != word && NotebookRows::CodeMatches(code))
        return NotebookRows::Instruction(vaddr, word);
    if (NativeNotebook::MapInstruction(vaddr, word) != word && NativeNotebook::MapCodeMatches(code))
        return NativeNotebook::MapInstruction(vaddr, word);
    if (NativeNotebook::Instruction(vaddr, word) != word && NativeNotebook::CodeMatches(code))
        return NativeNotebook::Instruction(vaddr, word);
    if (ItemPreview::Instruction(vaddr, word) != word && ItemPreview::CodeMatches(code))
        return ItemPreview::Instruction(vaddr, word);
    if (HudButtons::VisibilityInstruction(vaddr, word) != word && HudButtons::VisibilityCodeMatches(code))
        return HudButtons::VisibilityInstruction(vaddr, word);
    if (vaddr == SongList::NavPC && word == SongList::NavWord && SongList::CodeMatches(code))
        return 0xef000000 | SongList::NavSVC;
    if (DungeonMap::Instruction(vaddr, word) != word && DungeonMap::CodeMatches(code))
        return DungeonMap::Instruction(vaddr, word);
    if (OptionsMenu::Instruction(vaddr, word) != word && OptionsMenu::CodeMatches(code))
        return OptionsMenu::Instruction(vaddr, word);
    if (MenuShortcuts::Instruction(vaddr, word) != word && MenuShortcuts::CodeMatches(code))
        return MenuShortcuts::Instruction(vaddr, word);
    if (MenuClose::Instruction(vaddr, word) != word && MenuClose::CodeMatches(code))
        return MenuClose::Instruction(vaddr, word);
    if (BomberApproach::Instruction(vaddr, word) != word && BomberApproach::CodeMatches(code))
        return BomberApproach::Instruction(vaddr, word);
    if (OwlIntro::Instruction(vaddr, word) != word && OwlIntro::CodeMatches(code))
        return OwlIntro::Instruction(vaddr, word);
    if (OwlSword::Instruction(vaddr, word) != word && OwlSword::CodeMatches(code))
        return OwlSword::Instruction(vaddr, word);
    if (TatlReminders::Instruction(vaddr, word) != word && TatlReminders::CodeMatches(code))
        return TatlReminders::Instruction(vaddr, word);
    if (Music::Instruction(vaddr, word) != word && Music::CodeMatches(code))
        return Music::Instruction(vaddr, word);
    if (MenuSound::Instruction(vaddr, word) != word && MenuSound::CodeMatches(code))
        return MenuSound::Instruction(vaddr, word);
    if (ClassicDoubleTime::Instruction(vaddr, word) != word && ClassicDoubleTime::CodeMatches(code))
        return ClassicDoubleTime::Instruction(vaddr, word);
    if (Prices::Instruction(vaddr, word) != word && Prices::CodeMatches(code))
        return Prices::Instruction(vaddr, word);
    if (SingleScreenText::Instruction(vaddr, word) != word && SingleScreenText::CodeMatches(code))
        return SingleScreenText::Instruction(vaddr, word);
    if (MenuBackdrop::Instruction(vaddr, word) != word && MenuBackdrop::CodeMatches(code))
        return MenuBackdrop::Instruction(vaddr, word);
    if (MenuMemory::Instruction(vaddr, word) != word && MenuMemory::CodeMatches(code))
        return MenuMemory::Instruction(vaddr, word);
    if (MenuPageSwitch::Instruction(vaddr, word) != word && MenuPageSwitch::CodeMatches(code))
        return MenuPageSwitch::Instruction(vaddr, word);
    if (InventoryNavigation::Instruction(vaddr, word) != word && InventoryNavigation::CodeMatches(code))
        return InventoryNavigation::Instruction(vaddr, word);
    if (NativeItemPresent::IntroInstruction(vaddr, word) != word && NativeItemPresent::IntroCodeMatches(code))
        return NativeItemPresent::IntroInstruction(vaddr, word);
    if (VisibilityDepth::Instruction(vaddr, word) != word && VisibilityDepth::CodeMatches(code))
        return VisibilityDepth::Instruction(vaddr, word);
    if (PauseCapture::Instruction(vaddr, word) != word && PauseCapture::CodeMatches(code))
        return PauseCapture::Instruction(vaddr, word);
    if (NativeInventory::Instruction(vaddr, word) != word && NativeInventory::CodeMatches(code))
        return NativeInventory::Instruction(vaddr, word);
    if (DialogueScale::Instruction(vaddr, word) != word && DialogueScale::CodeMatches(code))
        return DialogueScale::Instruction(vaddr, word);
    if (NativeMinimap::Instruction(vaddr, word) != word && NativeMinimap::CodeMatches(code))
        return NativeMinimap::Instruction(vaddr, word);
    if (FormCamera::Instruction(vaddr, word) != word && FormCamera::CodeMatches(code))
        return FormCamera::Instruction(vaddr, word);
    if (CameraLive::ChangeModeInstruction(vaddr, word) != word && CameraLive::ChangeModeCodeMatches(code))
        return CameraLive::ChangeModeInstruction(vaddr, word);
    if (CameraLive::DialogueCameraInstruction(vaddr, word) != word && CameraLive::DialogueCameraCodeMatches(code))
        return CameraLive::DialogueCameraInstruction(vaddr, word);
    if (SongEffect::Instruction(vaddr, word) != word && SongEffect::CodeMatches(code))
        return SongEffect::Instruction(vaddr, word);
    if (CameraLive::ProjectionInstruction(vaddr, word) != word && CameraLive::CodeMatches(code, vaddr))
        return CameraLive::ProjectionInstruction(vaddr, word);
    if (CameraLive::AuxiliaryProjectionInstruction(vaddr, word) != word && CameraLive::CodeMatches(code))
        return CameraLive::AuxiliaryProjectionInstruction(vaddr, word);
    if (WideView::RestoreInstruction(vaddr, word) != word && CameraLive::CodeMatches(code))
        return WideView::RestoreInstruction(vaddr, word);
    if (WideView::LookAtInstruction(vaddr, word) != word && CameraLive::CodeMatches(code))
        return WideView::LookAtInstruction(vaddr, word);
    return word;
}

} // namespace MM3D
