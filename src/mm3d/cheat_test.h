// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include "common/logging/log.h"
#include "mm3d/simple_cheats.h"

// DEKUI_CHEAT_TEST=1: once a save is in play, runs Game Options > Cheats
// through the same requests as the panel and logs what each one wrote. Run it
// on a copy of the user folder (e.g. with DEKUI_SHADER_TOUR=1 and
// DEKUI_SHADER_TOUR_START=999 to load file 1 without touring).
namespace MM3D::CheatTest {
using namespace SimpleCheats;
inline bool configured=false, enabled=false;
inline int step=0, ticks=0, failures=0;
inline U32 start_scene=0, start_y=0, max_y=0;

inline float AsFloat(U32 bits){float f;std::memcpy(&f,&bits,4);return f;}
inline void Check(bool ok,const char* what,U32 value) {
    if(!ok)++failures;
    LOG_INFO(Frontend,"Cheat test: {} {} ({:#x})",ok?"PASS":"FAIL",what,value);
}
template<class M> std::optional<U32> Scene(M& m) {
    auto game=Game(m);
    return game?Get(m,*game+SceneOffset,2):std::nullopt;
}
template<class M> std::optional<U32> PlayerY(M& m) {
    auto game=Game(m);
    auto player=game?m.Read(*game+PlayerOffset):std::nullopt;
    return Pointer(player)?m.Read(*player+0x28):std::nullopt;
}

template<class M> void Tick(M& m,bool mm3d) {
    if(!configured){configured=true;const char* on=std::getenv("DEKUI_CHEAT_TEST");enabled=on&&!std::strcmp(on,"1");}
    if(!enabled||!mm3d||step<0)return;
    const bool playing=ready.load()&&InGame(m);
    ++ticks;
    auto next=[](){++step;ticks=0;};
    auto idle=[&]{auto g=Game(m);return playing&&g&&Get(m,*g+TransitionTrigger,1)==0u;};
    switch(step) {
    case 0: // Settle in play for 8 s (about 5.4 ticks a second).
        if(!playing)ticks=0;
        else if(ticks>43){LOG_INFO(Frontend,"Cheat test: start, scene {:#x}",Scene(m).value_or(0));next();}
        break;
    case 1:
        for(Action a:{Heal,Magic,DoubleMagic,Bank5000,Items,Masks,Gilded,Songs,Fairies,Bags,BomberCode,Lottery})Queue(a);
        give_item=0x17;max_hearts=20;set_time=0x8000;set_day=2;
        next();break;
    case 2:
        if(ticks<3)break;
        Check(result.load()==1,"one-shot requests applied",U32(result.load()));
        for(U32 i=0;i<MaskSlots.size();++i)if(Get(m,ItemSlots+24+i,1)!=MaskSlots[i]){Check(false,"mask slot",24+i);break;}
        Check(Get(m,ItemSlots+1,1)==1u&&Get(m,AmmoSlots+1,1).value_or(0)>=30,"bow with arrows",Get(m,AmmoSlots+1,1).value_or(0));
        Check(Get(m,SwordShield,1)==0x23u,"Gilded Sword + Mirror Shield",Get(m,SwordShield,1).value_or(0));
        Check(Get(m,MaxHealth,2)==320u&&Get(m,Health,2)==320u,"20 hearts",Get(m,MaxHealth,2).value_or(0));
        Check(Get(m,MagicAmount,1)==96u,"double magic full",Get(m,MagicAmount,1).value_or(0));
        Check(Get(m,BankRupees,2)==5000u,"bank 5000",Get(m,BankRupees,2).value_or(0));
        Check(m.Read(StrayFairies)==0x0f0f0f0fu,"stray fairies",m.Read(StrayFairies).value_or(0));
        Check(m.Read(SaveDay)==2u,"day 2",m.Read(SaveDay).value_or(0));
        {bool princess=false;for(U32 i=0;i<BottleCount;++i)princess|=Get(m,ItemSlots+BottleSlot+i,1)==0x17u;
         Check(princess,"Deku Princess bottled",0x17);}
        LOG_INFO(Frontend,"Cheat test: quest {:#010x} upgrades {:#010x} rupees {} time {:#06x} bomber {:#010x} lotto {:#010x}",
            m.Read(QuestStatus).value_or(0),m.Read(Upgrades).value_or(0),Get(m,Rupees,2).value_or(0),
            Get(m,SaveTime,2).value_or(0),m.Read(BomberCodeDigits+1).value_or(0),m.Read(LotteryCodes).value_or(0));
        continuous.fetch_or(1u<<FreezeTime);
        start_scene=Get(m,SaveTime,2).value_or(0);
        next();break;
    case 3: // Clock frozen for 3 s.
        if(ticks<16)break;
        {const U32 now=Get(m,SaveTime,2).value_or(0);
         Check(((now-start_scene)&0xffff)<0x20,"freeze time",now);}
        continuous.fetch_and(~(1u<<FreezeTime));
        next();break;
    case 4:next();break; // moon jump runs last, in an area entered normally
    case 5:
        if(!idle())break;
        start_scene=Scene(m).value_or(0);
        warp=int(0x43u<<9|U32(Deku)<<16|4u<<20); // Woodfall as Deku at midnight
        next();break;
    case 6:
        if(ticks>3&&idle()&&Scene(m)!=start_scene) {
            Check(true,"warp to Woodfall, scene",Scene(m).value_or(0));
            Check(Get(m,SaveForm,1)==U32(Deku),"arrived as Deku",Get(m,SaveForm,1).value_or(0));
            Check(Get(m,SaveTime,2).value_or(0x8000)<0x400,"arrived at midnight",Get(m,SaveTime,2).value_or(0));
            next();
        } else if(ticks>160){Check(false,"warp to Woodfall (timeout)",Scene(m).value_or(0));next();}
        break;
    case 7:
        if(ticks<16||!idle())break;
        boss=0; // Odolwa
        next();break;
    case 8:
        if(ticks>3&&idle()&&Scene(m)==0x1fu) {
            Check(boss_active.load(),"Odolwa rematch under way",Scene(m).value_or(0));
            next();
        } else if(ticks>160){Check(false,"Odolwa's Lair (timeout)",Scene(m).value_or(0));next();}
        break;
    case 9:
        if(ticks<27||!idle())break;
        warp=int(0x6cu<<9|U32(Human)<<16|2u<<20); // South Clock Town at noon
        next();break;
    case 10:
        if(ticks>3&&idle()&&Scene(m)!=0x1fu) {
            Check(!boss_active.load(),"rematch ended on leaving",Scene(m).value_or(0));
            Check(Get(m,SaveForm,1)==U32(Human),"back to human",Get(m,SaveForm,1).value_or(0));
            next();
        } else if(ticks>160){Check(false,"leave Odolwa's Lair (timeout)",Scene(m).value_or(0));next();}
        break;
    case 11: // Moon jump through the pad path for 1.5 s.
        if(!idle()||ticks<16){if(!idle())ticks=0;break;}
        if(ticks==16) {
            start_y=PlayerY(m).value_or(0);max_y=start_y;
            auto player=m.Read(*Game(m)+PlayerOffset);
            LOG_INFO(Frontend,"Cheat test: scene {:#x} player flags1 {:#x} gravity {:.2f}",Scene(m).value_or(0),
                player?m.Read(*player+PlayerFlags1).value_or(0):0,player?AsFloat(m.Read(*player+0x74).value_or(0)):0.f);
            continuous.fetch_or(1u<<MoonJump);test_moon_jump=true;
            break;
        }
        if(auto y=PlayerY(m);y&&AsFloat(*y)>AsFloat(max_y))max_y=*y;
        if(ticks>=24) {
            test_moon_jump=false;continuous.fetch_and(~(1u<<MoonJump));
            LOG_INFO(Frontend,"Cheat test: moon jump y {:.1f} -> max {:.1f}, {} writes",AsFloat(start_y),AsFloat(max_y),test_jump_writes.load());
            Check(AsFloat(max_y)>AsFloat(start_y)+40.f,"moon jump rises",max_y);
            next();
        }
        break;
    default:
        LOG_INFO(Frontend,"Cheat test: done, {} failures",failures);
        step=-1;
        break;
    }
}
} // namespace MM3D::CheatTest
