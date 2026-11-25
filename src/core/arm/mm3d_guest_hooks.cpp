// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project / Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef ENABLE_MM3D_PRESENTATION

#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <span>
#include <tuple>
#include "core/arm/mm3d_guest_hooks.h"
#include "core/core_timing.h"
#include "mm3d/code_substitutions.h"
#include "mm3d/wide_view.h"
#include "mm3d/interpolation_performance.h"

namespace Core {

std::optional<u32> MM3DGuestHooks::ReadCode(VAddr vaddr) {
    const auto instruction = memory.Read32OrNullopt(vaddr);
    if (!instruction)
        return instruction;
    struct CodeMemory {
        Memory::MemorySystem& memory;
        std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
    } code{memory};
    const u32 word = MM3D::SubstituteInstruction(code, vaddr, *instruction);
    if (word == *instruction)
        return instruction;
    const auto process = system.Kernel().GetCurrentProcess();
    if (process && process->codeset && process->codeset->program_id == MM3D::ProgramId)
        return word;
    return instruction;
}

bool MM3DGuestHooks::CallSVC(u32 swi) {
    if (swi == MM3D::VisibilityDepth::FirstSVC || swi == MM3D::VisibilityDepth::FirstSVC + 1) {
        auto& regs = cpu.Regs();
        const bool enabled = MM3D::CameraMidpoint::enabled.load();
        const auto generation = MM3D::CameraMidpoint::generation.load();
        if (swi == MM3D::VisibilityDepth::FirstSVC) {
            struct CodeMemory { Memory::MemorySystem& memory;
                std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
            } code{memory};
            visibility_depth_hook.Begin(code, enabled, MM3D::guest_frames.load(), generation);
            regs[0] = 0; // displaced MOV r0,#0; flags unchanged
        } else {
            const auto cached = visibility_depth_hook.Read(regs[2], regs[0], enabled, generation);
            if (cached) regs[0] = *cached;
            else {
                const u32 address = regs[2] + (regs[0] << 2);
                MM3D::InterpolationPerformance::GuestReadScope origin{
                    address, MM3D::VisibilityDepth::LoadPC, regs[14]};
                regs[0] = memory.Read32(address); // exact displaced LDR on every fallback
            }
        }
        return true;
    }
    if(swi==MM3D::ScreenShrink::GuardSVC) {
        auto& regs=cpu.Regs();
        regs[15]=MM3D::ScreenShrink::Guard(regs[5],regs[0],cpu.Cpsr(),regs[3]);
        return true;
    }
    if(swi==MM3D::ScreenShrink::SVC) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}
        } guest{memory};
        MM3D::ScreenShrink::Store(guest,cpu.Regs()[0],cpu.ExtRegs()[0],
            MM3D::Widescreen::supported_layout.load());
        return true;
    }

    if(swi>=MM3D::ClassicSaving::FirstSVC&&swi<MM3D::ClassicSaving::FirstSVC+MM3D::ClassicSaving::Sites.size()) {
        struct GuestMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        struct Service {
            bool Begin(unsigned slot,MM3D::ClassicSaving::Kind kind){return MM3D::ClassicSaving::BeginWrite(slot,kind);}
            bool Finish(unsigned slot,MM3D::ClassicSaving::Kind kind){return MM3D::ClassicSaving::FinishWrite(slot,kind);}
            void Load(unsigned slot,std::span<const std::uint8_t> bytes){MM3D::ClassicSaving::Resume(slot,bytes);}
        } service;
        const bool trace_owl=swi==MM3D::ClassicSaving::FirstSVC+15&&
            MM3D::OwlSword::QuietCompletion(guest,cpu.Regs()[4]);
        if(trace_owl) {
            owl_trace_actor=cpu.Regs()[4];owl_trace_play=cpu.Regs()[6];owl_trace_ticks=0;
            TraceOwlState("completion-before-release");
        }
        const bool trace_return=swi==MM3D::ClassicSaving::FirstSVC+17&&MM3D::ClassicSaving::ResumeNoticeArmed();
        const bool trace_return_finish=swi==MM3D::ClassicSaving::FirstSVC+16&&classic_save_hook.returning.player&&cpu.Regs()[0]==2;
        const bool trace_finish=swi==MM3D::ClassicSaving::FirstSVC+16&&classic_save_hook.quiet_player!=0;
        const u32 native_message_state=trace_finish?cpu.Regs()[0]:0;
        if(swi==MM3D::ClassicSaving::FirstSVC+19&&
           (MM3D::ClassicSaving::ResumeNoticeArmed()||classic_save_hook.returning.player)) {
            const auto& r=cpu.Regs();
            const u32 height=(guest.Read(r[0]).value_or(0)>>8)&255;
            const bool hidden=classic_save_hook.returning.HideFocusBars(guest);
            const u32 state=height|(hidden?0x100:0)|(MM3D::ClassicSaving::ResumeNoticeArmed()?0x200:0);
            static u32 last_state=~0u;
            if(state!=last_state) {
                last_state=state;
                LOG_INFO(Core_ARM11,"Classic owl return bars: height={} hidden={} pending={}",
                         height,hidden,MM3D::ClassicSaving::ResumeNoticeArmed());
            }
        }
        classic_save_hook.Handle(guest,cpu.Regs(),swi-MM3D::ClassicSaving::FirstSVC,service);
        if(trace_return&&!MM3D::ClassicSaving::ResumeNoticeArmed())
            LOG_INFO(Core_ARM11,"Classic owl return: {}",classic_save_hook.returning.player?"native conversation started before idle input":"unsupported message resource; notice skipped");
        if(trace_return_finish)LOG_INFO(Core_ARM11,"Classic owl return: native conversation finished; movement released");
        if(trace_finish)LOG_INFO(Core_ARM11,"Owl activation trace: conversation state native={} effective={}",native_message_state,cpu.Regs()[0]);
        if(trace_owl) {
            const auto& r=cpu.Regs();
            LOG_INFO(Core_ARM11,"Owl activation trace: quiet completion r0={:08x} r1={:08x} r2={} lr={:08x} pc={:08x}",r[0],r[1],r[2],r[14],r[15]);
        }
        if(MM3D::ClassicSaving::enabled && swi<=MM3D::ClassicSaving::FirstSVC+1) {
            const auto& regs=cpu.Regs();
            const u32 ctx=regs[swi==MM3D::ClassicSaving::FirstSVC?0:4];
            const u32 status=guest.Read(ctx+0x180).value_or(~0u);
            static u32 last_ctx=0,last_status=~0u;
            if(ctx!=last_ctx||status!=last_status) {
                last_ctx=ctx;last_status=status;
                LOG_INFO(Core_ARM11,"Classic cycle trace: ctx={:08x} state={:08x} status={} window={:08x} captured={:08x}",
                    ctx,guest.Read(ctx+0x13c).value_or(0),status,
                    guest.Read(ctx+0x174).value_or(0),classic_save_hook.cycle);
            }
        }
        return true;
    }
    if(swi==MM3D::MinimapShortcut::SVC) {
        struct GuestMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        MM3D::MinimapShortcut::Handle(guest,cpu.Regs());
        return true;
    }
    if(swi>=MM3D::BottleHealing::FirstSVC&&swi<MM3D::BottleHealing::FirstSVC+3) {
        struct GuestMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        MM3D::BottleHealing::Apply(guest,cpu.Regs(),swi-MM3D::BottleHealing::FirstSVC,
            MM3D::BottleHealing::red_hearts.load(),MM3D::BottleHealing::blue_hearts.load(),
            MM3D::BottleHealing::fairy_hearts.load());
        return true;
    }
    if(swi==MM3D::DeathPenalties::FirstSVC||swi==MM3D::DeathPenalties::FirstSVC+1) {
        struct GuestMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        if(swi==MM3D::DeathPenalties::FirstSVC)
            MM3D::DeathPenalties::Death(guest,cpu.Regs(),MM3D::DeathPenalties::lose_rupees.load());
        else
            MM3D::DeathPenalties::Continue(guest,cpu.Regs(),MM3D::DeathPenalties::world_ends.load());
        return true;
    }
    if(swi==MM3D::Difficulty::SpawnSVC) {
        struct GuestMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        MM3D::Difficulty::Spawn(guest,cpu.Regs(),MM3D::Difficulty::disable_heart_drops.load(),
                                MM3D::Difficulty::disable_rupee_drops.load(),
                                MM3D::Difficulty::disable_fairy_drops.load(),
                                MM3D::Difficulty::disable_magic_drops.load(),
                                MM3D::Difficulty::disable_ammo_pickups.load());
        return true;
    }
    if(swi>=MM3D::VegetationDrawDistance::FirstSVC&&swi<MM3D::VegetationDrawDistance::FirstSVC+MM3D::VegetationDrawDistance::Hooks.size()) {
        struct GuestMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        MM3D::VegetationDrawDistance::Load(guest,swi-MM3D::VegetationDrawDistance::FirstSVC,
            cpu.Regs(),cpu.ExtRegs(),MM3D::PropDrawDistance::multiplier.load());
        return true;
    }
    if(swi>=MM3D::PropDrawDistance::FirstSVC&&swi<MM3D::PropDrawDistance::FirstSVC+MM3D::PropDrawDistance::Hooks.size()) {
        MM3D::PropDrawDistance::Load(swi-MM3D::PropDrawDistance::FirstSVC,cpu.Regs(),
            cpu.ExtRegs(),MM3D::PropDrawDistance::multiplier.load());
        return true;
    }
    if(swi==MM3D::ActorDrawDistance::SVC) {
        struct GuestMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        auto& ext=cpu.ExtRegs();
        ext[0]=MM3D::ActorDrawDistance::Apply(guest,cpu.Regs()[4],ext[16],
                                           MM3D::ActorDrawDistance::multiplier.load());
        ext[0]=MM3D::PropDrawDistance::Apply(guest,cpu.Regs()[4],ext[0],
                                          MM3D::PropDrawDistance::multiplier.load());
        return true;
    }
    if(swi==MM3D::Difficulty::DefenseSVC) {
        auto& regs=cpu.Regs();
        regs[4]=MM3D::Difficulty::DefenseDelta(regs[4],regs[1],cpu.Cpsr(),
            MM3D::Difficulty::weaker_enhanced_defense.load());
        return true; // MOV without S: preserve CPSR and all other registers.
    }
    if(swi==MM3D::Difficulty::SVC) {
        auto& regs=cpu.Regs();
        regs[2]=MM3D::Difficulty::ApplyDamage(regs[2],regs[4],MM3D::Difficulty::damage_multiplier.load());
        return true; // displaced ADD does not alter CPSR or the other registers
    }
    if(swi==MM3D::NativeStartup::FirstSVC||swi==MM3D::NativeStartup::FirstSVC+1) {
        struct TitleMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& regs=cpu.Regs();
        if(swi==MM3D::NativeStartup::FirstSVC) {
            startup_hook.Enter(guest,regs[0]);
            link_pose_hook.Enter(guest,regs[0]);regs[5]=regs[0];
        } else {
            startup_hook.Matrix(guest,regs[5],regs[4]);
            link_pose_hook.MatrixBuilt(guest,regs[5],regs[4],MM3D::guest_frames.load());
            regs[0]=memory.Read32(regs[5]+0xc);
        }
        return true;
    }
    if(swi>=MM3D::ItemPreview::FirstSVC && swi<MM3D::ItemPreview::FirstSVC+MM3D::ItemPreview::PCs.size()) {
        struct PreviewMemory {
            Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
            void Write(u32 at,u32 value) { memory.Write32(at,value); }
        } guest{memory};
        auto& regs=cpu.Regs();
        switch(swi-MM3D::ItemPreview::FirstSVC) {
        case 0:
            item_preview_hook.Enter(guest,regs[0],regs[1]);
            // Shared Items/Masks entry runs after the complete icon array is built.
            inventory_hook.Capture(guest,regs[0]);
            regs[4]=regs[0]; break;
        case 1: item_preview_hook.Leave(regs[0]); regs[4]=regs[0]; break;
        case 2: item_preview_hook.ModelFrame(regs[0]); dungeon_map_hook.ModelFrame(guest,regs[0]); regs[4]=regs[0]; break;
        case 3: item_preview_hook.ModelBegin(guest,regs[4],regs[6]); dungeon_map_hook.preview.ModelBegin(guest,regs[4],regs[6]); regs[1]=regs[6]; break;
        case 4: dungeon_map_hook.preview.ModelEnd(guest); item_preview_hook.ModelEnd(guest); break; // displaced NOP
        case 5:
            item_preview_hook.EnterGear(guest,regs[0],regs[1]);
            inventory_hook.CaptureGear(guest,regs[0]);
            regs[4]=regs[0]; break;
        case 6:
            // This shared Gear Close instruction is dispatched through
            // ItemPreview first; release the backdrop's owned pause bit too.
            menu_backdrop.Release(guest);
            item_preview_hook.Leave(regs[0]);
            if(inventory_hook.gear_page==regs[0]) inventory_hook.Capture(guest,0);
            regs[5]=regs[0]; break;
        }
        return true;
    }
    if(swi==MM3D::ScheduleRows::FirstSVC||swi==MM3D::ScheduleRows::FirstSVC+1) {
        struct RowMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& regs=cpu.Regs();
        if(swi==MM3D::ScheduleRows::FirstSVC) {
            notebook_hook.schedule_rows.Enter(guest,regs[0]);regs[4]=regs[0];
        } else {
            regs[1]=0; // Native stop/reset path for Schedule vertical scrolling.
            regs[2]=0; // Displaced MOV r2,#0.
        }
        return true;
    }
    if(swi==MM3D::NotebookRows::SyncSVC) {
        struct RowMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        auto& regs=cpu.Regs();
        if(notebook_hook.row_window.ImmediateScroll(guest,regs[0],regs[2],regs[3])) {
            regs[2]|=8; // Populate the changed window now.
            regs[3]=0; // Native reset path, without the deferred scroll animation.
        }
        regs[4]=regs[0];return true;
    }
    if(swi==MM3D::NotebookRows::FirstSVC||swi==MM3D::NotebookRows::FirstSVC+1) {
        struct RowMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& regs=cpu.Regs();
        if(swi==MM3D::NotebookRows::FirstSVC){notebook_hook.row_window.Enter(guest,regs[0]);regs[4]=regs[0];}
        else {notebook_hook.row_window.Leave(guest);regs[0]=regs[4];}
        return true;
    }
    if(swi==MM3D::NativeNotebook::MapInputSVC||swi==0xffffd3) {
        struct NotebookMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);} } guest{memory};
        const auto& regs=cpu.Regs();
        cpu.SetCpsr(swi==0xffffd3 ?
            MM3D::NativeNotebook::ScheduleInputFlags(guest,regs[4],regs[3],cpu.Cpsr()) :
            MM3D::NativeNotebook::MapInputFlags(guest,regs[4],regs[3],cpu.Cpsr()));
        return true;
    }
    if(swi==MM3D::NativeNotebook::SVC) {
        struct NotebookMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);} } guest{memory};
        auto& regs=cpu.Regs();notebook_hook.Capture(guest,regs[0]);regs[4]=regs[0];return true;
    }
    if(swi==MM3D::SongList::NavSVC){
        auto& r=cpu.Regs();
        if(MM3D::SongList::enabled&&song_hook.panel&&memory.Read32OrNullopt(r[4]+0x14)==std::optional<u32>(song_hook.panel)){
            auto context=memory.Read32OrNullopt(r[7]+4);
            auto input=context?memory.Read32OrNullopt(*context+0x16c):std::optional<u32>{};
            auto a=input?memory.Read32OrNullopt(*input+0x34):std::optional<u32>{};
            auto b=input?memory.Read32OrNullopt(*input+0x8c):std::optional<u32>{};
            auto flags=memory.Read32OrNullopt(r[5]+0x1f8),mask=memory.Read32OrNullopt(r[6]+0x1c);
            if(a&&b&&flags&&mask)r[2]=MM3D::SongList::Selection(r[1],*a|*b,(*flags&*mask)!=0);
        }
        const u32 a=r[1],b=r[2],d=a-b;
        const u32 flags=(d&0x80000000u)|(d==0?0x40000000u:0)|(a>=b?0x20000000u:0)|(((a^b)&(a^d)&0x80000000u)>>3);
        cpu.SetCpsr((cpu.Cpsr()&0x0fffffffu)|flags);return true;
    }
    if(swi==MM3D::BomberApproach::FirstSVC||swi==MM3D::BomberApproach::FirstSVC+1) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        auto& r=cpu.Regs();
        const bool suppress=MM3D::BomberApproach::Suppress(guest,r[4]);
        if(swi==MM3D::BomberApproach::FirstSVC) {
            const auto original=static_cast<u32>(static_cast<s32>(static_cast<s16>(memory.Read16(r[5]+0x8c))));
            r[0]=MM3D::BomberApproach::State(original,suppress);
        } else if(suppress)r[15]=MM3D::BomberApproach::ResetPC;
        else r[1]=0xf;
        return true;
    }
    if(swi==MM3D::OwlIntro::FirstSVC||swi==MM3D::OwlIntro::FirstSVC+1) {
        auto& r=cpu.Regs();
        if(swi==MM3D::OwlIntro::FirstSVC)MM3D::OwlIntro::Enter(r);
        else r[0]=MM3D::OwlIntro::TatlFlags(memory.Read8(r[0]+0x2d3));
        return true;
    }
    if(swi==MM3D::OwlSword::FirstSVC||swi==MM3D::OwlSword::FirstSVC+1) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& r=cpu.Regs();
        if(swi==MM3D::OwlSword::FirstSVC) {
            r[4]=r[0];
            const auto before=guest.Read(r[4]+0x268);
            MM3D::OwlSword::BlockFeatherAcceptance(guest,r[4],MM3D::ClassicSaving::enabled.load());
            MM3D::OwlSword::Update(guest,r[4]);
            if(before!=guest.Read(r[4]+0x268)&&guest.Read(r[4]+0x268)==MM3D::OwlSword::Activate) {
                owl_trace_actor=r[4];owl_trace_play=r[1];owl_trace_ticks=0;
                TraceOwlState("sword-hit");
            }
            if(owl_trace_actor==r[4]&&owl_trace_play==r[1]&&owl_trace_ticks<180) {
                ++owl_trace_ticks;
                if(owl_trace_ticks<=3||owl_trace_ticks%30==0)TraceOwlState("actor-update");
            }
        }
        else if(MM3D::OwlSword::Locked(guest,r[0]) ||
                MM3D::OwlSword::FeatherBlocked(guest,r[0],MM3D::ClassicSaving::enabled.load()))r[0]=0;
        else {r[14]=MM3D::OwlSword::OfferPC+4;r[15]=0x3c322c;}
        return true;
    }
    if(swi==MM3D::TatlReminders::SVC) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& r=cpu.Regs();
        r[4]=MM3D::TatlReminders::Request(guest,r[0],r[1]);return true;
    }
    if(swi==MM3D::Music::SVC) {
        struct SoundMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 word){memory.Write32(at,word);}} guest{memory};
        const auto& regs=cpu.Regs();
        MM3D::Music::Store(guest,regs[4],regs[5],cpu.ExtRegs()[16]);return true;
    }
    if(swi==MM3D::MenuSound::SVC) {
        struct SoundMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& r=cpu.Regs();
        MM3D::MenuSound::Store(guest,menu_sound_context,r[4],r[0]);return true;
    }
    if(swi>=MM3D::ClassicDoubleTime::FirstSVC&&swi<MM3D::ClassicDoubleTime::FirstSVC+MM3D::ClassicDoubleTime::Sites.size()) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        const auto site=swi-MM3D::ClassicDoubleTime::FirstSVC;
        const bool owned=classic_double_time_hook.target.has_value();
        auto& r=cpu.Regs();
        classic_double_time_hook.Handle(guest,r,site);
        if(site==0&&MM3D::ClassicDoubleTime::enabled.load())
            LOG_INFO(Core_ARM11,"Classic Double Time: dispatch PC={:08X}, question={}, message_data={:08X}",
                r[15],classic_double_time_hook.target?int(classic_double_time_hook.target->question):-1,
                classic_double_time_hook.message_data);
        else if(owned&&(site==2||site==3))
            LOG_INFO(Core_ARM11,"Classic Double Time: choice site={}, continuation={:08X}",site,r[15]);
        return true;
    }
    if(swi>=MM3D::Prices::FirstSVC&&swi<MM3D::Prices::FirstSVC+MM3D::Prices::Sites.size()) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        MM3D::Prices::Execute(guest,cpu.Regs(),swi-MM3D::Prices::FirstSVC,MM3D::Prices::multiplier.load());
        return true;
    }
    if(swi==MM3D::SingleScreenText::SVC) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& r=cpu.Regs();
        classic_double_time_hook.Message(guest,r[0],r[1]);
        MM3D::ClassicSaving::ObserveMessage(r[1]);
        MM3D::SingleScreenText::Apply(guest,r[0],r[1]);
        if(MM3D::Prices::CodeMatches(guest))MM3D::Prices::Message(guest,r[0],r[1],MM3D::Prices::multiplier.load());
        r[3]=memory.Read32(r[0]+4); // original LDR r3,[r0,#4]
        return true;
    }
    if(swi>=MM3D::MenuBackdrop::FirstSVC&&swi<MM3D::MenuBackdrop::FirstSVC+MM3D::MenuBackdrop::PCs.size()) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& r=cpu.Regs();
        switch(swi-MM3D::MenuBackdrop::FirstSVC) {
        case 0:
            menu_backdrop.Pause(guest,r[7],r[4],r[1],r[6]);
            LOG_INFO(Core_ARM11,"MM3D Gear backdrop: full_pause={} captured_scene={}",
                (guest.Read(r[7]+0xaac).value_or(0)&0xffff)==3,menu_backdrop.Active(guest));
            break;
        case 1: case 2: menu_backdrop.Pause(guest,r[5],r[4],r[1],r[0]);break;
        case 3: menu_backdrop.Release(guest);r[5]=r[0];break;
        case 4: menu_backdrop.CloseMap(guest,r,menu_page_switch.KeepCapture(guest,MM3D::MenuBackdrop::Hook::Game(guest,r[1])+0x11010));break;
        }
        return true;
    }
    if(swi>=MM3D::MenuMemory::FirstSVC&&swi<MM3D::MenuMemory::FirstSVC+MM3D::MenuMemory::PCs.size()) {
        struct Guest {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& r=cpu.Regs();
        if(swi==MM3D::MenuMemory::FirstSVC) {
            if(!menu_memory.Prepare(guest,r)&&
               !(MM3D::MenuBackdrop::CodeMatches(guest)&&menu_backdrop.Prepare(guest,r))) {
                r[15]=r[2]&~1u;
                cpu.SetCpsr((cpu.Cpsr()&~0x20u)|((r[2]&1u)<<5));
            }
        } else if(swi==MM3D::MenuMemory::FirstSVC+1)menu_memory.Ready(guest,r[4],menu_backdrop.PreparedReady(guest,r[4],r[0]));
        else if(!(MM3D::MenuBackdrop::CodeMatches(guest)&&menu_backdrop.KeepGearCapture(guest,r[0],r[4],r[7])))
            menu_memory.GearOpenCapture(guest,r[0],r[6],r[4],r[7]);
        return true;
    }
    if(swi>=MM3D::MenuPageSwitch::FirstSVC&&swi<MM3D::MenuPageSwitch::FirstSVC+MM3D::MenuPageSwitch::PCs.size()) {
        struct PageMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& r=cpu.Regs();
        switch(swi-MM3D::MenuPageSwitch::FirstSVC) {
        case 0:
            menu_sound_context=r[0];
            menu_backdrop.Tick(guest);
            if(MM3D::MenuMemory::CodeMatches(guest))menu_memory.Tick(guest,r[0]);
            menu_page_switch.Tick(guest,r[0]);r[4]=r[0];break;
        case 1:
            r[14]=MM3D::MenuPageSwitch::PCs[1]+4;
            if(menu_page_switch.PairPending(guest,r[1]))r[0]=1;
            else {r[15]=r[2]&~1u;cpu.SetCpsr((cpu.Cpsr()&~0x20u)|((r[2]&1u)<<5));}
            break;
        case 2:
            if(menu_page_switch.FinishLayout(guest,r[0]))cpu.ExtRegs()[0]=std::bit_cast<u32>(10.f);
            r[4]=r[0];break;
        case 6: cpu.ExtRegs()[1]=MM3D::MenuBackdrop::CodeMatches(guest)&&menu_backdrop.Active(guest)?0:MM3D::MenuPageSwitch::GearBackdropAlpha();break;
        case 7: menu_page_switch.MapCloseSound(guest,r);break;
        default: menu_page_switch.CaptureCall(guest,r,swi-MM3D::MenuPageSwitch::FirstSVC);break;
        }
        return true;
    }
    if(swi>=MM3D::InventoryNavigation::FirstSVC&&swi<=MM3D::InventoryNavigation::FirstSVC+2) {
        struct NavMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& regs=cpu.Regs();const bool vertical=swi==MM3D::InventoryNavigation::FirstSVC;
        if(swi==MM3D::InventoryNavigation::FirstSVC+2) {
            regs[2]|=regs[12];
            if(auto next=MM3D::InventoryNavigation::GearMove(guest,regs[0],regs[2])) {
                const auto current=memory.Read8(regs[0]+0x195);
                regs[2]=*next;regs[15]=*next==current?0x5a56e0:0x5a574c;
            }
            return true;
        }
        if(MM3D::InventoryNavigation::PresentMove(guest,regs[4],regs[13],vertical,regs[5])||
           MM3D::InventoryNavigation::Move(guest,regs[4],regs[13],vertical,regs[5]))
            regs[15]=vertical?0x5a0eb0:0x5a14c0;
        else regs[1]=memory.Read8(regs[4]+(vertical?0x19:0x18));
        return true;
    }
    if(swi==MM3D::NativeItemPresent::IntroSVC) {
        struct ReadMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        auto& regs=cpu.Regs();
        regs[14]=MM3D::NativeItemPresent::IntroPC+4;
        if(MM3D::NativeItemPresent::SkipIntro(guest,regs[5])) {
            regs[0]=1;regs[15]=regs[14]; // Native caller closes the message and advances focus.
        } else regs[15]=0x1adb90; // Replay the original BL for unsupported/disabled cases.
        return true;
    }
    if(swi>=MM3D::PauseCapture::FirstSVC&&swi<MM3D::PauseCapture::FirstSVC+3) {
        struct ReadMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        auto& regs=cpu.Regs();
        switch(swi-MM3D::PauseCapture::FirstSVC) {
        case 0: pause_capture_hook.Convert(guest,regs[7],regs[8],regs[4],regs[5]);regs[0]=regs[7]+(regs[8]<<2);break;
        case 1: pause_capture_hook.Bind(guest,regs[3],regs[2]);regs[12]=regs[0]+0x1c;break;
        case 2: pause_capture_hook.Reset();regs[4]=regs[0];break;
        }
        return true;
    }
    if(swi>=MM3D::NativeInventory::FirstSVC && swi<MM3D::NativeInventory::FirstSVC+4) {
        struct InventoryMemory {
            Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
            void Write(u32 at,u32 value) { memory.Write32(at,value); }
        } guest{memory};
        auto& regs=cpu.Regs();
        switch(swi-MM3D::NativeInventory::FirstSVC) {
        case 0: inventory_hook.Capture(guest,regs[0]); regs[4]=regs[0]; break;
        case 1: MM3D::ScreenShrink::Update(guest); if(MM3D::MenuBackdrop::CodeMatches(guest))menu_backdrop.Pass(guest,regs[0],regs[1]);
            options_hook.Pass(guest,regs[0],regs[1]); song_reset_hook.Pass(guest,regs[0],regs[1]); double_time_hook.Pass(guest,regs[0],regs[1]); dungeon_map_hook.Pass(guest,regs[0],regs[1]); song_hook.Pass(guest,regs[0],regs[1]); ocarina_hook.Pass(guest,regs[0],regs[1]); item_present_hook.Pass(guest,regs[0],regs[1]); press_hook.Pass(guest,regs[0],regs[1]); inventory_hook.Pass(guest,regs[0],regs[1]); notebook_hook.Pass(guest,regs[0],regs[1]); file_select_hook.Pass(guest,regs[0],regs[1]);
            if(regs[1]==5&&file_select_hook.route_status!=file_select_hook.last_route_status) {
                LOG_INFO(Core_ARM11,"MM3D FileSelect: active={} stage={} panel={:08x} summary={:08x} popup={:08x}",
                    file_select_hook.active,file_select_hook.route_status,file_select_hook.panel,file_select_hook.summary,file_select_hook.popup);
                file_select_hook.last_route_status=file_select_hook.route_status;
            }
            banner_hook.Pass(guest,regs[0],regs[1]); regs[4]=regs[0]; break;
        case 2: classic_save_hook.BeginCycleDraw(guest,regs[4]); minigame_hook.Begin(guest,regs[4]); hud_buttons_hook.Begin(guest,regs[4]); song_reset_hook.Begin(guest,regs[4]); banner_hook.Begin(guest,regs[4],MM3D::Widescreen::Active()); double_time_hook.Begin(guest,regs[4]); dungeon_map_hook.Begin(guest,regs[4]); song_hook.Begin(guest,regs[4]); item_present_hook.Begin(guest,regs[4]); loading_icon_hook.Begin(guest,regs[4]); press_hook.Begin(guest,regs[4]); inventory_hook.Begin(guest,regs[4],MM3D::Widescreen::Active()); notebook_hook.Begin(guest,regs[4],MM3D::Widescreen::Active()); file_select_hook.Begin(guest,regs[4],MM3D::Widescreen::Active());
            if(MM3D::ClassicSaving::enabled && classic_save_hook.cycle) {
                static std::vector<u32> traced_layouts;
                static u32 traced_cycle=0;
                if(traced_cycle!=classic_save_hook.cycle) {
                    traced_cycle=classic_save_hook.cycle;traced_layouts.clear();
                }
                if(traced_layouts.size()<32 && std::find(traced_layouts.begin(),traced_layouts.end(),regs[4])==traced_layouts.end()) {
                    traced_layouts.push_back(regs[4]);
                    const auto window=guest.Read(traced_cycle+0x174).value_or(0);
                    LOG_INFO(Core_ARM11,"Classic cycle draw: layout={:08x} message={} owned={:08x} inner={:08x} flags={:08x} hidden={}",
                        regs[4],MM3D::NativeInventory::LayoutNamed(guest,regs[4],"MessageWindow"),window,
                        window?guest.Read(window+0x358).value_or(0):0,
                        guest.Read(regs[4]+8+0xd4).value_or(0),
                        !classic_save_hook.cycle_draws.empty() && classic_save_hook.cycle_draws.back().address!=0);
                }
            }
            options_hook.Begin(guest,regs[4]);
            player_hud_hook.Begin(guest,regs[4]);
            regs[2]=notebook_hook.PromptRoot(guest,regs[4],regs[13]);
            {const auto root=ocarina_hook.Root(guest,regs[4],regs[13],MM3D::Widescreen::Active());
             if(root!=regs[4]+8)regs[2]=root;}
            {const auto root=magic_hook.Root(guest,regs[4],regs[13]);
             if(root!=regs[4]+8)regs[2]=root;}
            {const auto root=song_hook.ShadowRoot(guest,regs[4],regs[13]);
             if(root!=regs[4]+8)regs[2]=root;}
            {const auto content=regs[2];
             regs[2]=song_hook.HeaderRoot(guest,regs[4],content,content!=regs[4]+8?content:regs[13]);}
            {const auto content=regs[2];
             regs[2]=song_hook.PanelRoot(guest,regs[4],content,content!=regs[4]+8?content:regs[13]);}
            {const auto root=inventory_shadow_hook.Root(guest,inventory_hook,regs[4],regs[13],&item_present_hook);
             if(root!=regs[4]+8)regs[2]=root;}
            items_map_prompt_hook.ResolveDescription(guest,item_preview_hook.owner,inventory_hook.panel);
            {const auto existing_root=regs[2];
             const auto prompt_sp=existing_root!=regs[4]+8?existing_root:regs[13];
             const auto root=items_map_prompt_hook.Root(guest,inventory_hook,regs[4],
                prompt_sp,existing_root,MM3D::Widescreen::Active(),
                system.CoreTiming().GetGlobalTimeUs().count());
             if(root!=regs[4]+8)regs[2]=root;}
            {const auto root=dungeon_map_hook.PromptRoot(guest,regs[4],regs[13],MM3D::Widescreen::Active());
             if(root!=regs[4]+8)regs[2]=root;}
            {const auto root=dungeon_map_hook.PanelRoot(guest,regs[4],regs[13]);
             if(root!=regs[4]+8)regs[2]=root;}
            {const auto root=dungeon_map_hook.ShadowRoot(guest,regs[4],regs[13]);
             if(root!=regs[4]+8)regs[2]=root;}
            regs[2]=map_items_prompt_hook.MapRoot(guest,dungeon_map_hook,regs[4],
                regs[2]!=regs[4]+8?regs[2]:regs[13],regs[2],MM3D::Widescreen::Active(),
                system.CoreTiming().GetGlobalTimeUs().count());
            regs[2]=world_map_prompt_hook.Root(guest,dungeon_map_hook,regs[4],
                regs[2]!=regs[4]+8?regs[2]:regs[13],regs[2],MM3D::Widescreen::Active());
            regs[2]=menu_tabs_hook.Root(guest,inventory_hook,regs[4],
                regs[2]!=regs[4]+8?regs[2]:regs[13],regs[2]);
            menu_help_hook.source.ResolveDescription(guest,item_preview_hook.owner,inventory_hook.panel);
            regs[2]=menu_help_hook.Root(guest,inventory_hook,regs[4],
                regs[2]!=regs[4]+8?regs[2]:regs[13],regs[2]);
            if(!item_present_hook.IsDescription(guest,select_help_hook.source.description))select_help_hook.source.description=0;
            regs[2]=select_help_hook.Root(guest,inventory_hook,regs[4],
                regs[2]!=regs[4]+8?regs[2]:regs[13],regs[2],&item_present_hook);
            regs[2]=file_select_prompt_hook.Root(guest,file_select_hook,options_hook,regs[4],
                regs[2]!=regs[4]+8?regs[2]:regs[13],regs[2],MM3D::Widescreen::Active());
            regs[2]=classic_file_marker.Root(guest,file_select_hook,regs[4],regs[2],
                regs[2]!=regs[4]+8?regs[2]:regs[13],
                file_select_hook.active?MM3D::ClassicSaving::PendingMask():0,
                system.CoreTiming().GetGlobalTimeUs().count());
            regs[2]=classic_owl_draw.Root(guest,regs[4],regs[2],
                regs[2]!=regs[4]+8?regs[2]:regs[13],
                classic_save_hook.presentation.window,classic_save_hook.presentation.frames);
            {
                static unsigned last_owl_draw_status=0;
                const auto status=MM3D::ClassicSaving::owl_draw_status.load();
                if(status!=last_owl_draw_status) {
                    LOG_INFO(Core_ARM11,"Classic owl draw: status={} (layout=1 labels=2 cursor=4 fade=8)",status);
                    last_owl_draw_status=status;
                }
            }
            regs[2]=double_time_hook.ShadeRoot(guest,regs[4],regs[2],
                regs[2]!=regs[4]+8?regs[2]:regs[13]);
            if(regs[2]!=regs[4]+8)regs[13]=regs[2];
            break;
        case 3:
            if(const auto sp=double_time_hook.shade_scratch.End(guest))regs[13]=sp;
            classic_save_hook.EndCycleDraw(guest);
            if(const auto sp=classic_owl_draw.End(guest))regs[13]=sp;
            if(const auto sp=classic_file_marker.scratch.End(guest))regs[13]=sp;
            if(const auto sp=file_select_prompt_hook.End(guest))regs[13]=sp;
            if(const auto sp=select_help_hook.scratch.End(guest))regs[13]=sp;
            if(const auto sp=menu_help_hook.scratch.End(guest))regs[13]=sp;
            if(const auto sp=menu_tabs_hook.scratch.End(guest))regs[13]=sp;
            if(const auto sp=world_map_prompt_hook.End(guest))regs[13]=sp;
            if(const auto sp=map_items_prompt_hook.prompt.End(guest))regs[13]=sp;
            if(const auto sp=dungeon_map_hook.shadow_scratch.End(guest))regs[13]=sp;
            if(const auto sp=dungeon_map_hook.panel_scratch.End(guest))regs[13]=sp;
            if(const auto sp=dungeon_map_hook.map_prompts.scratch.End(guest))regs[13]=sp;
            if(const auto sp=items_map_prompt_hook.prompt.End(guest))regs[13]=sp;
            if(const auto sp=inventory_shadow_hook.scratch.End(guest))regs[13]=sp;
            if(const auto sp=song_hook.backdrop.scratch.End(guest))regs[13]=sp;
            if(const auto sp=song_hook.header.scratch.End(guest))regs[13]=sp;
            if(const auto sp=song_hook.shadow_scratch.End(guest))regs[13]=sp;
            if(const auto sp=magic_hook.End(guest))regs[13]=sp;
            if(const auto sp=ocarina_hook.End(guest))regs[13]=sp;
            if(const auto sp=notebook_hook.prompt_hook.End(guest))regs[13]=sp;
            player_hud_hook.End(guest);
            options_hook.End(guest);
            song_hook.End(guest); item_present_hook.End(guest); file_select_hook.End(guest); notebook_hook.End(guest); inventory_hook.End(guest); press_hook.End(guest); loading_icon_hook.End(guest); dungeon_map_hook.End(guest); double_time_hook.End(guest); banner_hook.End(guest); song_reset_hook.End(guest); hud_buttons_hook.End(guest); minigame_hook.End(guest); guest.Write(regs[6],regs[0]); break;
        }
        return true;
    }
    if(swi==MM3D::DialogueScale::BeforeSVC || swi==MM3D::DialogueScale::AfterSVC) {
        struct PoseMemory {
            Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
            void Write(u32 at,u32 value) { memory.Write32(at,value); }
        } pose{memory};
        const auto layout=cpu.Regs()[4];
        if(swi==MM3D::DialogueScale::BeforeSVC) {
            const bool description=item_preview_hook.Description(pose,layout);
            const bool present_description=item_present_hook.IsDescription(pose,layout);
            if(present_description){select_help_hook.source.description=layout;
                select_help_hook.source.description_panel=item_present_hook.panel;}
            const auto& setting=MM3D::NativeItemPresent::poses[MM3D::NativeItemPresent::Description];
            const MM3D::GearMenu::Pose description_pose{setting.x.load(),setting.y.load(),setting.scale.load()};
            const bool map_description=dungeon_map_hook.IsDescription(pose,layout);
            const auto& map_setting=MM3D::DungeonMap::poses[MM3D::DungeonMap::Description];
            const MM3D::GearMenu::Pose map_pose{map_setting.x.load(),map_setting.y.load(),map_setting.scale.load()};
            const bool reset_description=song_reset_hook.IsDescription(pose,layout);
            const auto& reset_setting=MM3D::SongReset::poses[MM3D::SongReset::Description];
            const MM3D::GearMenu::Pose reset_pose{reset_setting.x.load(),reset_setting.y.load(),reset_setting.scale.load()};
            const bool options_description=options_hook.IsDescription(pose,layout);
            dialogue_hook.Begin(pose,layout,description,description&&item_preview_hook.gear,
                options_description?&options_hook.frame.description:reset_description?&reset_pose:map_description?&map_pose:present_description?&description_pose:nullptr);
            cpu.Regs()[0]=layout+8; // displaced ADD r0,r4,#8
        } else {
            dialogue_hook.End(pose,layout);
            cpu.Regs()[13]+=8; // displaced ADD sp,sp,#8
        }
        return true;
    }
    if(swi>=MM3D::DungeonMap::FirstSVC&&swi<MM3D::DungeonMap::FirstSVC+MM3D::DungeonMap::PCs.size()) {
        struct MapMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& regs=cpu.Regs();
        switch(swi-MM3D::DungeonMap::FirstSVC) {
        case 0: dungeon_map_hook.Capture(guest,regs[0],regs[1]);regs[4]=regs[0];break;
        case 1:
            dungeon_map_hook.BeforeMap(guest);
            regs[14]=MM3D::DungeonMap::PCs[1]+4;regs[15]=0x2a326c;break;
        case 2: dungeon_map_hook.AfterMap(guest);regs[0]=memory.Read8(regs[4]+0x74c);break;
        case 3:
            if(dungeon_map_hook.valid) MM3D::DungeonMap::LogInput(guest,regs[4],memory.Read8(regs[6]+0xc));
            regs[0]=memory.Read8(regs[6]+0xc);
            regs[0]=MM3D::DungeonMap::FilterA(guest,regs[4],regs[0]);
            break;
        case 4:
            regs[0]=memory.Read32(regs[4]+0x1c);
            if(dungeon_map_hook.SkipWorld(guest,regs[4]))regs[15]=MM3D::DungeonMap::WorldEnd;
            break;
        }
        return true;
    }
    if(swi==MM3D::OptionsMenu::DrawSVC||swi==MM3D::OptionsMenu::SelectSVC) {
        struct CodeMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        auto& regs=cpu.Regs();
        if(swi==MM3D::OptionsMenu::DrawSVC)options_hook.Capture(guest,regs[0]);
        else regs[2]=MM3D::OptionsMenu::Selection(guest,regs[0],regs[2]);
        regs[4]=regs[0];return true;
    }
    if (swi==MM3D::NativeMinimap::BeforeSVC || swi==MM3D::NativeMinimap::AfterSVC) {
        struct MapMemory {
            Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
            void Write(u32 at,u32 value) { memory.Write32(at,value); }
        } map_memory{memory};
        const auto screen=cpu.Regs()[4];
        if (swi==MM3D::NativeMinimap::BeforeSVC) {
            minimap_hook.Begin(map_memory,screen,MM3D::Minimap::enabled.load(),cpu.Regs()[0]);
        } else {
            minimap_hook.End(map_memory,screen,MM3D::Minimap::enabled.load(),
                MM3D::Minimap::scale.load(),MM3D::Minimap::opacity.load(),
                MM3D::Widescreen::Active() ? 400.f+(240.f*16.f/9.f-400.f)/2.f : 400.f);
            // Emulate the displaced LDR r0,[r4,#0x38], including paths which skip map drawing.
            cpu.Regs()[0]=memory.Read32(screen+0x38);
        }
        return true;
    }
    if(swi>=MM3D::SongEffect::FirstSVC&&swi<MM3D::SongEffect::FirstSVC+MM3D::SongEffect::Loads.size()) {
        struct EffectMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}} guest{memory};
        auto& regs=cpu.Regs();
        MM3D::SongEffect::Expand(guest,regs[13],MM3D::CameraLive::desired_fov.load(),
            MM3D::Widescreen::Active(),MM3D::Widescreen::camera_zoom_out.load());
        regs[1]=regs[13]; // displaced MOV, including guarded fallback
        return true;
    }
    if(swi==MM3D::FormCamera::EntrySVC||swi==MM3D::FormCamera::RadiusSVC||swi==MM3D::FormCamera::LockSVC) {
        struct CameraMemory {Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 word){memory.Write32(at,word);}} guest{memory};
        auto& regs=cpu.Regs();
        if(swi==MM3D::FormCamera::EntrySVC) {
            const bool native_entry_orbit=entry_recovery_orbit.Native(guest,regs[0]);
            form_camera.Entry(guest,regs,native_entry_orbit);
            const auto deku_lens_before=guest.Read(regs[0]+0x110).value_or(0);
            deku_free_camera.Step(guest,regs[0]);
            if(MM3D::FormCamera::diagnostics.load()) {
                const auto actor=guest.Read(regs[0]+0xa4).value_or(0);
                LOG_INFO(Core_ARM11,"Deku free camera: armed={} recovering={} flags3={:08x} ground={:08x} lens_before={} lens_after={}",
                    deku_free_camera.armed,deku_free_camera.recovering,
                    actor?guest.Read(actor+0x11db8).value_or(0):0,
                    actor?guest.Read(actor+0x94).value_or(0):0,
                    std::bit_cast<float>(deku_lens_before),
                    std::bit_cast<float>(guest.Read(regs[0]+0x110).value_or(0)));
            }
            const bool ocarina_before=ocarina_return_lens.pending||ocarina_return_lens.active;
            const auto ocarina_bits=ocarina_before?guest.Read(regs[0]+0x110).value_or(0):0;
            ocarina_return_lens.Step(guest,regs[0]);
            if(ocarina_before&&MM3D::FormCamera::diagnostics.load())
                LOG_INFO(Core_ARM11,"Ocarina return lens: cam={:08x} active={} before={} after={}",
                    regs[0],ocarina_return_lens.active,std::bit_cast<float>(ocarina_bits),
                    std::bit_cast<float>(guest.Read(regs[0]+0x110).value_or(0)));
            const bool arrival_before=arrival_lens.active;
            const auto arrival_bits=guest.Read(regs[0]+0x110).value_or(0);
            const auto expected_bits=arrival_lens.last_bits;
            arrival_lens.Step(guest,regs[0]);
            if((arrival_before||arrival_lens.active)&&MM3D::FormCamera::diagnostics.load())
                LOG_INFO(Core_ARM11,"Arrival lens step: active={} setting={} normal={} before={} expected={} after={} rate={} orbit={} orbit_target={}",
                    arrival_lens.active,guest.Read(regs[0]+0x154).value_or(0)>>16,
                    arrival_lens.NormalFov(guest,guest.Read(regs[0]+0x154).value_or(0)>>16).value_or(-1.f),
                    std::bit_cast<float>(arrival_bits),std::bit_cast<float>(expected_bits),
                    std::bit_cast<float>(guest.Read(regs[0]+0x110).value_or(0)),
                    std::bit_cast<float>(guest.Read(regs[0]+0xe8).value_or(0)),
                    arrival_lens.radius,arrival_lens.radius_target);
            if(MM3D::FormCamera::diagnostics.load())
                LOG_INFO(Core_ARM11,"Form camera entry: cam={:08x} phase={:08x} target={:08x} radius={} saved={} native_entry_orbit={}",
                    regs[4],guest.Read(regs[4]+0x170).value_or(0),guest.Read(regs[4]+0xbc).value_or(0),
                    form_camera.radius,form_camera.saved,native_entry_orbit);
        }
        else if(swi==MM3D::FormCamera::RadiusSVC) {
            const auto before=guest.Read(regs[13]);
            form_camera.Radius(guest,regs);
            arrival_lens.Radius(guest,regs,form_camera);
            if(MM3D::FormCamera::diagnostics.load()) {
                const auto after=guest.Read(regs[13]);
                LOG_INFO(Core_ARM11,"Form camera orbit: cam={:08x} native={} applied={} saved={}",
                    regs[4],before?std::bit_cast<float>(*before):0.f,
                    after?std::bit_cast<float>(*after):0.f,form_camera.saved);
            }
        } else {
            const auto requested=regs[1];
            const auto cam=regs[0];
            form_camera.LockMode(guest,regs);
            if(MM3D::FormCamera::diagnostics.load()) {
                const auto target=form_camera.Target(guest,cam,true);
                const u32 player=target?(*target)[0]:0;
                LOG_INFO(Core_ARM11,"Form camera lock request: cam={:08x} requested={} mode={} native_lock_allowed={} camera_flags={:08x} camera_target={:08x} player_flags={:08x} player_target={:08x}",
                    cam,requested,guest.Read(cam+0x158).value_or(0)&65535,
                    regs[15]==regs[14]&&regs[0]==0,
                    guest.Read(cam+0x170).value_or(0),guest.Read(cam+0xbc).value_or(0),
                    player?guest.Read(player+0x11db0).value_or(0):0,
                    player?guest.Read(player+0xdfc).value_or(0):0);
            }
        }
        return true;
    }
    if(swi==MM3D::FormCamera::SVC) {
        struct CameraMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        const auto cam=cpu.Regs()[4];
        const float before_s0=std::bit_cast<float>(cpu.ExtRegs()[0]);
        const float before_s1=std::bit_cast<float>(cpu.ExtRegs()[1]);
        const auto mode=guest.Read(cam+0x158),native=guest.Read(cam+0xf0);
        form_camera.Distance(guest,cam,cpu.ExtRegs());
        MM3D::FormCamera::DekuFreeCamera::Distance(guest,cam,cpu.ExtRegs());
        const bool capture=MM3D::FormCamera::diagnostics.load();
        if(capture&&!form_trace_was_active) {form_trace_count=form_projection_count=0;form_last_native=form_last_output=form_last_radius=-1.f;form_last_mode=~0u;}
        form_trace_was_active=capture;
        if(capture) {
            ++form_trace_count;
            LOG_INFO(Core_ARM11,"Form camera distance: cam={:08x} mode={} native_radius={} s0_before={} s1_before={} s0_after={} s1_after={} saved_radius={} saved={}",
                cam,mode?(*mode&65535):999u,native?std::bit_cast<float>(*native):0.f,
                before_s0,before_s1,std::bit_cast<float>(cpu.ExtRegs()[0]),
                std::bit_cast<float>(cpu.ExtRegs()[1]),form_camera.radius,form_camera.saved);
        }
        return true;
    }
    if(swi==MM3D::CameraLive::ChangeModeSVC) {
        struct CameraMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        const auto cam=cpu.Regs()[0];
        const auto requested=cpu.Regs()[1];
        const auto mode_before=guest.Read(cam+0x158);
        const auto effective=form_camera.RequestedMode(guest,cam,requested);
        cpu.Regs()[1]=effective;
        const bool arrival_before=arrival_lens.active;
        arrival_lens.Observe(guest,cam,effective);
        ocarina_return_lens.Observe(guest,cam,effective);
        form_camera.ChangeMode(guest,cam,effective);
        entry_recovery_orbit.Observe(guest,cam,effective);
        deku_free_camera.Observe(cam,effective);
        if(MM3D::FormCamera::diagnostics.load()) {
            const auto player=guest.Read(cam+0xa4).value_or(0);
            LOG_INFO(Core_ARM11,"Arrival lens mode: before={} active={} setting={} normal={} native={} ground={:08x} requested={}",
                arrival_before,arrival_lens.active,guest.Read(cam+0x154).value_or(0)>>16,
                arrival_lens.NormalFov(guest,guest.Read(cam+0x154).value_or(0)>>16).value_or(-1.f),
                std::bit_cast<float>(guest.Read(cam+0x110).value_or(0)),
                player?guest.Read(player+0x94).value_or(0):0,effective);
            ++form_trace_count;
            LOG_INFO(Core_ARM11,"Form camera mode: cam={:08x} requested={} mode_before={} effective={} radius={} saved={}",
                cam,requested,mode_before?(*mode_before&65535):999u,effective,form_camera.radius,form_camera.saved);
        }
        cpu.Regs()[4]=cam+0x100; // displaced ADD; native mode selection retained
        return true;
    }
    if(swi>=MM3D::WideView::LookAtSVC && swi<MM3D::WideView::LookAtSVC+MM3D::WideView::LookAtSites.size()) {
        // Another LookAt of the live view: turn its target before the call,
        // put it back after (mm3d/wide_view.h).
        const auto& site=MM3D::WideView::LookAtSites[swi-MM3D::WideView::LookAtSVC];
        struct ViewMemory {
            Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}
        } view_memory{memory};
        auto& regs=cpu.Regs();
        // The second eye/at/up (+50) is turned as a view at +28.
        auto& state=site.second?MM3D::WideView::second_view_rotation:MM3D::WideView::view_rotation;
        const u32 shift=site.second?0x28:0;
        if(site.turn) {
            // LookAt(matrix, eye r1, up r2, at r3), all from one view.
            const u32 view=regs[1]-0x28-shift;
            if((MM3D::WideView::enabled.load() || state.valid) &&
               regs[3]==view+shift+0x34 && regs[2]==view+shift+0x40 &&
               view_memory.Read(MM3D::CameraLive::CameraPointer)==std::optional<u32>(view)) {
                MM3D::WideView::RotateView(view_memory,view+shift,state);
                state.core=cpu.CoreId();
            }
        } else if(state.valid) {
            MM3D::WideView::RestoreView(view_memory,state.view,state);
        }
        MM3D::WideView::EmulateLookAtAdd(site,regs);
        return true;
    }
    if(swi==MM3D::WideView::RestoreSVC) {
        // After the game's LookAt: emulate the replaced `add r1, r4, #380`.
        struct ViewMemory {
            Memory::MemorySystem& memory;
            std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
            void Write(u32 at,u32 value){memory.Write32(at,value);}
        } view_memory{memory};
        MM3D::WideView::RestoreView(view_memory,cpu.Regs()[4]);
        cpu.Regs()[1]=cpu.Regs()[4]+380;
        return true;
    }
    if(swi==MM3D::CameraLive::AuxiliaryProjectionSVC) {
        // Emulate the replaced VLDR of the auxiliary FOV; widened only in the
        // immersive view (mm3d/camera_live.h).
        const u32 native=memory.Read32(cpu.Regs()[4]+0x4c);
        const float configured=MM3D::WideView::enabled.load()&&!MM3D::WideView::menu.load() ?
            std::clamp(MM3D::WideView::fov.load(),MM3D::WideView::MinFov,MM3D::WideView::MaxFov) : 45.f;
        const auto output=MM3D::CameraLive::ProjectionValue(native,configured);
        cpu.ExtRegs()[0]=output;
        if(MM3D::WideView::enabled.load()) {
            static std::pair<int,int> aux_last{};
            static unsigned aux_lines=0;
            const std::pair<int,int> now{int(std::lround(std::bit_cast<float>(native))),
                                         int(std::lround(std::bit_cast<float>(output)))};
            if(now!=aux_last && aux_lines<200) {
                aux_last=now;++aux_lines;
                LOG_INFO(Core_ARM11,"Wide view auxiliary projection: {:08x} fov {} -> {}",
                    cpu.Regs()[4],now.first,now.second);
            }
        }
        return true;
    }
    if(swi==MM3D::CameraLive::ProjectionSVC) {
        // SVC is a synchronization boundary: R4 and S0 are live here.
        // Emulate the replaced VLDR, changing only its destination register.
        const u32 native=memory.Read32(cpu.Regs()[4]+0x18);
        struct CameraMemory {Memory::MemorySystem& memory;std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}} guest{memory};
        const auto projection=cpu.Regs()[4];
        const float configured=MM3D::CameraLive::ConfiguredFov(guest,projection);
        const auto output=MM3D::CameraLive::ProjectionValue(native,configured);
        cpu.ExtRegs()[0]=output;
        // Immersive view diagnostics: each projection the hook widens, on change.
        if(MM3D::WideView::enabled.load()) {
            static std::tuple<u32,bool,int,int,bool> wide_last{};
            static unsigned wide_lines=0;
            const bool live=memory.Read32(MM3D::CameraLive::CameraPointer)==projection;
            const std::tuple<u32,bool,int,int,bool> now{projection,live,
                int(std::lround(std::bit_cast<float>(native))),int(std::lround(std::bit_cast<float>(output))),
                MM3D::CameraLive::ScriptedCamera(guest)};
            if(now!=wide_last && wide_lines<400) {
                wide_last=now;++wide_lines;
                LOG_INFO(Core_ARM11,"Wide view projection: {:08x} live={} fov {} -> {} scripted={} first_person={}",
                    projection,live,std::get<2>(now),std::get<3>(now),std::get<4>(now),
                    MM3D::WideView::first_person.load());
            }
        }
        // Dekui's immersive view turns the live view in place with the head,
        // before this function's LookAt reads it (mm3d/wide_view.h).
        if(MM3D::WideView::enabled.load() || MM3D::WideView::view_rotation.valid) {
            struct ViewMemory {
                Memory::MemorySystem& memory;
                std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
                void Write(u32 at,u32 value){memory.Write32(at,value);}
            } view_memory{memory};
            if(view_memory.Read(MM3D::CameraLive::CameraPointer)==std::optional<u32>(projection)) {
                MM3D::WideView::RotateView(view_memory,projection);
                MM3D::WideView::view_rotation.core=cpu.CoreId();
            }
        }
        // Manual form capture remains available for all supported forms, including Deku.
        if(MM3D::FormCamera::diagnostics.load()) {
            const auto ctx=memory.Read32(0x72f5c0);
            std::optional<u32> game,player,form;
            if(ctx) game=memory.Read32(ctx+0x16c);
            if(game) player=memory.Read32(*game+0x20dc);
            if(player) form=memory.Read32(*player+0x1fc);
            const bool form_match=form&&(MM3D::FormCamera::SupportedForm(*form>>24)||(*form>>24)==3);
            if(form_match&&memory.Read32(MM3D::CameraLive::CameraPointer)==projection) {
                const auto indices=game?memory.Read32(*game+0xa78):std::optional<u32>{};
                const unsigned index=indices?(*indices>>16):99;
                const auto camera=(game&&index<4)?(*game+0x408+index*0x198):0;
                const auto mode=camera?memory.Read32(camera+0x158):std::optional<u32>{};
                const auto radius=camera?memory.Read32(camera+0xf0):std::optional<u32>{};
                const u32 mode_value=mode?(u32(*mode)&65535):999u;
                const float native_value=std::bit_cast<float>(native),output_value=std::bit_cast<float>(output),
                    radius_value=radius?std::bit_cast<float>(*radius):0.f;
                std::array<u32,6> pose{};
                for(unsigned axis=0;axis<6;++axis)pose[axis]=memory.Read32(projection+0x28+axis*4);
                const bool changed=pose!=form_last_pose||mode_value!=form_last_mode||std::abs(native_value-form_last_native)>.05f||
                    std::abs(output_value-form_last_output)>.05f||std::abs(radius_value-form_last_radius)>.05f;
                if(changed) {
                    ++form_projection_count;form_last_pose=pose;form_last_mode=mode_value;form_last_native=native_value;
                    form_last_output=output_value;form_last_radius=radius_value;
                    LOG_INFO(Core_ARM11,"Form camera projection: form={} mode={} native_fov={} configured={} output_fov={} radius={} projection={:08x} eye=({},{},{}) at=({},{},{}) target={:08x} flags={:08x} camera={:08x} index={} setting={}",
                        *form>>24,mode_value,native_value,configured,output_value,radius_value,projection,
                        std::bit_cast<float>(pose[0]),std::bit_cast<float>(pose[1]),std::bit_cast<float>(pose[2]),
                        std::bit_cast<float>(pose[3]),std::bit_cast<float>(pose[4]),std::bit_cast<float>(pose[5]),
                        camera?guest.Read(camera+0xbc).value_or(0):0,camera?guest.Read(camera+0x170).value_or(0):0,
                        camera,index,camera?guest.Read(camera+0x154).value_or(0)>>16:0);
                }
            }
        }
        return true;
    }
    return false;
}

u32 MM3DGuestHooks::Read32(VAddr vaddr) {
    if(vaddr>=Memory::VRAM_VADDR && vaddr<Memory::VRAM_VADDR_END &&
       MM3D::InterpolationPerformance::pending.load(std::memory_order_relaxed)) {
        const auto& regs=cpu.Regs();
        MM3D::InterpolationPerformance::GuestReadScope origin{vaddr,regs[15],regs[14]};
        return memory.Read32(vaddr);
    }
    return memory.Read32(vaddr);
}

void MM3DGuestHooks::BeforeRun(Memory::PageTable* page_table) {
    if(MM3D::CameraMidpoint::enabled.load() || MM3D::CameraLive::enabled.load() || MM3D::CameraLive::capture_requested.load() || MM3D::CameraLive::desired_fov.load()>45.f || MM3D::CameraLive::needs_service.load()) {
        const auto process=system.Kernel().GetCurrentProcess();
        if(process && process->codeset && process->codeset->program_id==0x0004000000125500ULL) {
            struct CameraMemory {
                Memory::MemorySystem& memory;
                std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }


            } camera_memory{memory};
            MM3D::CameraLive::Service(camera_memory,page_table,std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }
    // A turned view the RestoreSVC hook did not restore (translated AOT code
    // predating it): restore it once this core has left the view function.
    for(auto* state:{&MM3D::WideView::view_rotation,&MM3D::WideView::second_view_rotation}) {
        if(state->valid && state->core==cpu.CoreId() &&
           !MM3D::WideView::InViewFunction(cpu.Regs()[15])) {
            struct ViewMemory {
                Memory::MemorySystem& memory;
                std::optional<u32> Read(u32 at){return memory.Read32OrNullopt(at);}
                void Write(u32 at,u32 value){memory.Write32(at,value);}
            } view_memory{memory};
            MM3D::WideView::RestoreView(view_memory,state->view,*state);
        }
    }
    // Dekui's immersive view lets gyro aiming turn first-person cameras and
    // steers the others with the C-stick (mm3d/wide_view.h).
    if(MM3D::WideView::enabled.load()) {
        const auto process=system.Kernel().GetCurrentProcess();
        if(process && process->codeset && process->codeset->program_id==0x0004000000125500ULL) {
            struct CameraMemory {
                Memory::MemorySystem& memory;
                std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
            } camera_memory{memory};
            MM3D::WideView::first_person.store(
                MM3D::WideView::FirstPersonCamera(camera_memory).value_or(false));
            const u32 screen=MM3D::WideView::CurrentScreen(camera_memory);
            MM3D::WideView::menu.store(MM3D::WideView::MenuScreen(screen));
            // Each screen the game shows, to find menus not covered yet.
            static u32 logged_screen=~0u;
            static unsigned screen_lines=0;
            if(screen!=logged_screen && screen_lines<200) {
                logged_screen=screen;++screen_lines;
                LOG_INFO(Core_ARM11,"Wide view screen: vtable {:08x} menu={}",screen,
                    MM3D::WideView::menu.load());
            }
        }
    } else {
        MM3D::WideView::menu.store(false);
    }
}

void MM3DGuestHooks::AfterRun(Memory::PageTable* page_table) {
    if (MM3D::NotebookInspector::enabled.load() && page_table) {
        struct Pages {
            Memory::MemorySystem& memory;
            Memory::PageTable& table;
            size_t size() const { return Memory::PAGE_TABLE_NUM_ENTRIES; }
            u8* operator[](size_t i) {
                if (table.attributes[i] == Memory::PageType::RasterizerCachedMemory)
                    return memory.GetPointer(static_cast<u32>(i << 12));
                return table.GetPointerArray()[i];
            }
        } pages{memory, *page_table};
        MM3D::NotebookInspector::Poll(pages, page_table);
    }
}

} // namespace Core

#endif
