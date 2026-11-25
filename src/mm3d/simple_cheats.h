// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

// Game Options > Cheats. The save cheats follow the usual MM3D code lists
// (CTRPF/Gateway); warps, boss rematches, freeze time and moon jump follow
// MM3D-PC's F1 > Cheats and the shader tour's native void-respawn warp.
namespace MM3D::SimpleCheats {
using U32=std::uint32_t;
enum Action : unsigned { FillWallet, Rupees0, Heal, Magic, DoubleMagic, Bank5000, Bank0,
    Items, Masks, Gilded, Razor, Songs, Fairies, Bags, GreatFairySwordB, FierceDeityB,
    BomberCode, Lottery, Count };
inline constexpr std::array<const char*,Count> Labels{
    "Fill wallet", "0 rupees", "Refill hearts", "Refill magic",
    "Double magic + enhanced defense", "5000 rupees in the bank", "0 rupees in the bank",
    "All items", "All masks", "Mirror Shield + Gilded Sword", "Mirror Shield + Razor Sword",
    "All boss remains and songs", "All stray fairies", "Largest quiver, bomb bag and wallet",
    "Great Fairy's Sword on B", "Fierce Deity's Mask on B",
    "Bombers' code = 12345", "Lottery numbers = 777"};
inline constexpr std::array<const char*,Count> Help{
    "Fills the wallet you have (99, 200 or 500 rupees).", "Clears carried rupees and pending rupee changes.",
    "Refills health to your current maximum.", "Refills the magic meter if you have already unlocked magic.",
    "Unlocks magic with the double meter and enhanced defense, and refills magic.",
    "Sets your bank savings to the 5000-rupee maximum. Deposit to collect rewards you missed.",
    "Clears your bank savings.",
    "Adds the usable items with some ammo, and empty bottles in the bottle slots you don't have yet. Bottle contents are kept.",
    "Adds all 24 masks.", "Sets sword and shield equipment; reopen the equipment screen to refresh it.",
    "Sets sword and shield equipment; reopen the equipment screen to refresh it.",
    "Grants the four remains and song flags; preserves heart pieces and notebook progress.",
    "Gives all 15 stray fairies of each temple.",
    "Upgrades quiver, bomb bag and wallet capacity; preserves stick and nut upgrades.",
    "Puts the Great Fairy's Sword on Link's B button until you change equipment.",
    "Puts the Fierce Deity's Mask on Link's B button: press B to transform anywhere.",
    "Sets this cycle's Bombers' Notebook code to 1-2-3-4-5.",
    "Sets this cycle's lottery numbers to 7-7-7 for all three days."};
struct Ammo {const char* label;U32 count;U32 slot;U32 item;U32 amount;};
// USA Restoration uses the 0x775xxx save layout, not the supplied v1.1
// 0x776xxx layout. Inventory IDs and equipment masks match game/common_data.h.
inline constexpr std::array<Ammo,7> Ammunition{{
    {"Infinite arrows",0x775391,0x775355,1,99},
    {"Infinite bombs",0x775396,0x77535a,6,99},
    {"Infinite Bombchus",0x775397,0x77535b,7,99},
    {"Infinite Deku nuts",0x775399,0x77535d,9,50},
    {"Infinite Deku sticks",0x775398,0x77535c,8,99},
    {"Infinite magic beans",0x77539a,0x77535e,10,99},
    {"Infinite Powder Kegs",0x77539c,0x775360,12,99}
}};
// Continuous cheats after the seven ammo bits.
enum Toggle : unsigned { InfiniteHealth=7, InfiniteMagic, InfiniteRupees, UnbreakableRazor,
    FreezeTime, SlowTime, MoonJump, ToggleCount };
inline constexpr std::array<std::pair<const char*,const char*>,ToggleCount-InfiniteHealth> Toggles{{
    {"Infinite health","Refills to your current capacity. Turning it off stops refills."},
    {"Infinite magic","Refills to your current capacity. Turning it off stops refills."},
    {"Infinite rupees","Keeps the wallet you have full."},
    {"Unbreakable Razor Sword","Keeps the Razor Sword at 100 hits."},
    {"Freeze time","Stops the three-day clock. Song of Time, Song of Double Time and new days still move it."},
    {"Slow time","Runs the clock as slowly as the Inverted Song of Time."},
    {"Moon jump (hold L + A)","Hold L and A to rise. A is not passed to the game while L is held."}
}};

// Save data (USA Restoration).
constexpr U32 SaveMask=0x7751e0, SaveIsNight=0x7751e4, TimeSpeed=0x7751e8, SaveDay=0x7751ec;
constexpr U32 SaveTime=0x7751f8, PendingRupees=0x7751fc, SaveForm=0x7751fe;
constexpr U32 MaxHealth=0x775312, Health=0x775314, MagicLevel=0x775316, MagicAmount=0x775317;
constexpr U32 Rupees=0x775318, RazorHits=0x77531a, MagicAcquired=0x77531e, DoubleMagicFlag=0x77531f;
constexpr U32 DoubleDefense=0x775320, ButtonItems=0x77532a, SwordShield=0x775352;
constexpr U32 ItemSlots=0x775354, AmmoSlots=0x775390, Upgrades=0x7753cc, QuestStatus=0x7753d0;
constexpr U32 StrayFairies=0x7753e8, BankRupees=0x776408, WeekEvents=0x776428;
constexpr U32 LotteryCodes=0x776558, BomberCodeDigits=0x776567;
// Play state (see shader_tour.h).
constexpr U32 UiContext=0x72f5c0, GameOffset=0x16c, StateOffset=0x13c, SceneOffset=0x148;
constexpr U32 NextEntrance=0xc52e, TransitionTrigger=0xc529, TransitionType=0xc534;
constexpr U32 TransitionUnknown=0xc535, PlayerOffset=0x20dc;
// Player actor: velocity y +0x68 (from MM3D-PC). Its state flags at +0x11db0
// do not mean what MM3D-PC assumes here (0x20000000 is set while Link walks),
// so moon jump does not check them.
constexpr U32 PlayerVelocityY=0x68, PlayerFlags1=0x11db0;

// Item slot for each mask, in slot order from slot 24 (the game's slot table
// at 0x6a0b48).
inline constexpr std::array<std::uint8_t,24> MaskSlots{0x3e,0x38,0x47,0x45,0x40,0x32,0x3a,0x46,
    0x39,0x42,0x48,0x33,0x3c,0x3d,0x37,0x3f,0x36,0x34,0x43,0x41,0x3b,0x44,0x49,0x35};
constexpr U32 BottleSlot=48, BottleCount=6;

struct Item {std::uint8_t id;const char* name;};
inline constexpr Item GiveItems[]{
    {0x00,"Ocarina of Time"},{0x01,"Hero's Bow"},{0x02,"Fire Arrow"},{0x03,"Ice Arrow"},
    {0x04,"Light Arrow"},{0x06,"Bombs"},{0x07,"Bombchus"},{0x08,"Deku Sticks"},{0x09,"Deku Nuts"},
    {0x0a,"Magic Beans"},{0x0c,"Powder Keg"},{0x0d,"Pictograph Box"},{0x0e,"Lens of Truth"},
    {0x0f,"Hookshot"},{0x10,"Great Fairy's Sword"},
    {0x12,"Empty Bottle"},{0x13,"Red Potion"},{0x14,"Green Potion"},{0x15,"Blue Potion"},
    {0x16,"Fairy"},{0x17,"Deku Princess"},{0x18,"Milk"},{0x19,"Milk (half)"},{0x1a,"Fish"},
    {0x1b,"Bug"},{0x1d,"Poe"},{0x1e,"Big Poe"},{0x1f,"Spring Water"},{0x20,"Hot Spring Water"},
    {0x21,"Zora Egg"},{0x22,"Gold Dust"},{0x23,"Magic Mushroom"},{0x24,"Seahorse"},
    {0x25,"Chateau Romani"},{0x26,"Hylian Loach"},
    {0x28,"Moon's Tear"},{0x29,"Land Title Deed"},{0x2a,"Swamp Title Deed"},
    {0x2b,"Mountain Title Deed"},{0x2c,"Ocean Title Deed"},{0x2d,"Room Key"},
    {0x2e,"Express Mail to Mama"},{0x2f,"Letter to Kafei"},{0x30,"Pendant of Memories"},
    {0x32,"Deku Mask"},{0x33,"Goron Mask"},{0x34,"Zora Mask"},{0x35,"Fierce Deity's Mask"},
    {0x36,"Mask of Truth"},{0x37,"Kafei's Mask"},{0x38,"All-Night Mask"},{0x39,"Bunny Hood"},
    {0x3a,"Keaton Mask"},{0x3b,"Garo's Mask"},{0x3c,"Romani's Mask"},{0x3d,"Circus Leader's Mask"},
    {0x3e,"Postman's Hat"},{0x3f,"Couple's Mask"},{0x40,"Great Fairy's Mask"},{0x41,"Gibdo Mask"},
    {0x42,"Don Gero's Mask"},{0x43,"Kamaro's Mask"},{0x44,"Captain's Hat"},{0x45,"Stone Mask"},
    {0x46,"Bremen Mask"},{0x47,"Blast Mask"},{0x48,"Mask of Scents"},{0x49,"Giant's Mask"}};

// Warp destinations: entrance scene groups (entrance = group << 9, spawn 0),
// named as the MM decomp's entrance table, which the 3DS game shares. The
// shader tour visits all of them; 0x08, 0x09, 0x0e and 0x5f are cutscene or
// unused maps and 0x1d freezes on a black screen, so they are left out.
enum Region : unsigned { ClockTown, Field, Swamp, Mountain, Ocean, Canyon, Moon, Other, RegionCount };
inline constexpr std::array<const char*,RegionCount> RegionNames{
    "Clock Town","Termina Field and Romani Ranch","Southern Swamp and Woodfall",
    "Snowhead","Great Bay","Ikana","The Moon","Other"};
struct Destination {std::uint8_t group;Region region;const char* name;};
inline constexpr Destination Destinations[]{
    {0x6c,ClockTown,"South Clock Town"},{0x6b,ClockTown,"North Clock Town"},
    {0x69,ClockTown,"East Clock Town"},{0x6a,ClockTown,"West Clock Town"},
    {0x6d,ClockTown,"Laundry Pool"},{0x60,ClockTown,"Clock Tower interior"},
    {0x16,ClockTown,"Clock Tower rooftop"},{0x00,ClockTown,"Mayor's Residence"},
    {0x5e,ClockTown,"Stock Pot Inn"},{0x12,ClockTown,"Milk Bar"},
    {0x07,ClockTown,"Curiosity Shop"},{0x31,ClockTown,"Trading Post"},
    {0x65,ClockTown,"Bomb Shop"},{0x14,ClockTown,"Treasure Chest Shop"},
    {0x04,ClockTown,"Honey & Darling's Shop"},{0x36,ClockTown,"Lottery Shop"},
    {0x2b,ClockTown,"Post Office"},{0x51,ClockTown,"Swordsman's School"},
    {0x1b,ClockTown,"Deku Scrub Playground"},{0x26,ClockTown,"Astral Observatory"},
    {0x2a,Field,"Termina Field"},{0x1f,Field,"Milk Road"},{0x67,Field,"Gorman Track"},
    {0x32,Field,"Romani Ranch"},{0x03,Field,"Ranch House and Barn"},
    {0x3e,Field,"Doggy Racetrack"},{0x3f,Field,"Cucco Shack"},
    {0x3d,Swamp,"Road to Southern Swamp"},{0x42,Swamp,"Southern Swamp (poisoned)"},
    {0x06,Swamp,"Southern Swamp (clear)"},{0x54,Swamp,"Tourist Information"},
    {0x02,Swamp,"Magic Hags' Potion Shop"},{0x21,Swamp,"Swamp Shooting Gallery"},
    {0x61,Swamp,"Woods of Mystery"},{0x28,Swamp,"Deku Palace"},
    {0x3b,Swamp,"Deku King's Chamber"},{0x4f,Swamp,"Deku Shrine"},
    {0x24,Swamp,"Swamp Spider House"},{0x43,Swamp,"Woodfall"},
    {0x18,Swamp,"Woodfall Temple"},{0x1c,Swamp,"Odolwa's Lair"},
    {0x19,Mountain,"Path to Mountain Village"},{0x4d,Mountain,"Mountain Village (winter)"},
    {0x57,Mountain,"Mountain Village (spring)"},{0x29,Mountain,"Mountain Smithy"},
    {0x5a,Mountain,"Path to Goron Village (winter)"},{0x5b,Mountain,"Path to Goron Village (spring)"},
    {0x4a,Mountain,"Goron Village (winter)"},{0x45,Mountain,"Goron Village (spring)"},
    {0x2f,Mountain,"Goron Shrine"},{0x3a,Mountain,"Goron Shop"},
    {0x4b,Mountain,"Goron Graveyard"},{0x68,Mountain,"Goron Racetrack"},
    {0x58,Mountain,"Path to Snowhead"},{0x59,Mountain,"Snowhead"},
    {0x1e,Mountain,"Snowhead Temple"},{0x41,Mountain,"Goht's Lair"},
    {0x34,Ocean,"Great Bay Coast"},{0x35,Ocean,"Zora Cape"},{0x39,Ocean,"Fisherman's Hut"},
    {0x2c,Ocean,"Marine Research Lab"},{0x25,Ocean,"Oceanside Spider House"},
    {0x47,Ocean,"Waterfall Rapids"},{0x38,Ocean,"Pirates' Fortress exterior"},
    {0x11,Ocean,"Pirates' Fortress"},{0x20,Ocean,"Pirates' Fortress interior"},
    {0x22,Ocean,"Pinnacle Rock"},{0x30,Ocean,"Zora Hall"},{0x49,Ocean,"Zora Hall rooms"},
    {0x46,Ocean,"Great Bay Temple"},{0x5c,Ocean,"Gyorg's Lair"},
    {0x50,Canyon,"Road to Ikana"},{0x40,Canyon,"Ikana Graveyard"},
    {0x05,Canyon,"Beneath the Graveyard"},{0x2d,Canyon,"Dampe's House"},
    {0x10,Canyon,"Ikana Canyon"},{0x52,Canyon,"Music Box House"},{0x4e,Canyon,"Ghost Hut"},
    {0x4c,Canyon,"Sakon's Hideout"},{0x48,Canyon,"Beneath the Well"},
    {0x1a,Canyon,"Ancient Castle of Ikana"},{0x53,Canyon,"Igos du Ikana's Lair"},
    {0x5d,Canyon,"Secret Shrine"},{0x55,Canyon,"Stone Tower"},
    {0x56,Canyon,"Stone Tower (inverted)"},{0x13,Canyon,"Stone Tower Temple"},
    {0x15,Canyon,"Stone Tower Temple (inverted)"},{0x33,Canyon,"Twinmold's Lair"},
    {0x64,Moon,"The Moon"},{0x27,Moon,"Deku Trial"},{0x3c,Moon,"Goron Trial"},
    {0x44,Moon,"Zora Trial"},{0x63,Moon,"Link Trial"},{0x01,Moon,"Majora's Lair"},
    {0x62,Other,"Lost Woods (opening)"},{0x17,Other,"Clock Tower basement (opening)"},
    {0x23,Other,"Fairy Fountain"},{0x0a,Other,"Grottos"},{0x66,Other,"Giants' Chamber"}};
enum Form : U32 { FierceDeity=0, Goron=1, Zora=2, Deku=3, Human=4, KeepForm=7 };
inline constexpr std::array<const char*,6> FormNames{"Current form","Human","Deku","Goron","Zora","Fierce Deity"};
inline constexpr std::array<U32,6> FormChoices{KeepForm,Human,Deku,Goron,Zora,FierceDeity};
constexpr U32 MaskFor(U32 form) {
    return form==Deku?0x18:form==Goron?0x16:form==Zora?0x17:form==FierceDeity?0x15:0;
}
// Arrival time choices: index 0 keeps the clock.
inline constexpr std::array<const char*,5> TimeNames{"Current time","6:00 AM","Noon","6:00 PM","Midnight"};
inline constexpr std::array<U32,5> TimeValues{0,0x4000,0x8000,0xc000,0x0000};

struct Boss {const char* name;U32 entrance_group;U32 scene;int week_event;U32 week_mask;int remains;};
// Their lairs and the "temple cleared" flags that keep them away once beaten
// (MM3D-PC). Igos du Ikana uses a room flag that resets each cycle.
inline constexpr std::array<Boss,6> Bosses{{
    {"Odolwa",0x1c,0x1f,20,0x02,0},{"Goht",0x41,0x44,33,0x80,1},
    {"Gyorg",0x5c,0x5f,55,0x80,2},{"Twinmold",0x33,0x36,52,0x20,3},
    {"Igos du Ikana",0x53,0x56,-1,0,-1},{"Majora",0x01,0x0b,-1,0,-1}}};

inline std::atomic<U32> pending{0}, continuous{0};
// One-shot requests (-1 none). warp: entrance | form << 16 | time choice << 20.
inline std::atomic<int> warp{-1}, give_item{-1}, max_hearts{-1}, set_time{-1}, set_day{-1};
inline std::atomic<int> boss{-1};
inline std::atomic<bool> keep_boss_win{false}, boss_active{false};
inline std::atomic<bool> ready{false};
inline std::atomic<bool> test_moon_jump{false}; // cheat_test.h holds L + A
inline std::atomic<U32> test_jump_writes{0};
inline std::atomic<int> result{0}; // 0 idle, 1 applied, -1 not in a supported save, -2 item had no room.
// Emulation thread only.
struct BossFight {int scene=-1,ticks=0;bool arrived=false;U32 flag_was=0,quest_was=0,hearts_was=0;const Boss* boss=nullptr;};
inline BossFight fight;
inline U32 frozen_time=0;
inline bool time_frozen=false, slowed=false;
inline void Reset(){pending=0;continuous=0;warp=-1;give_item=-1;max_hearts=-1;set_time=-1;
    set_day=-1;boss=-1;boss_active=false;fight={};time_frozen=false;slowed=false;ready=false;result=0;}
inline void Queue(Action a){if(a<Count)pending.fetch_or(1u<<a);}
inline bool Requested(){return pending.load()||warp.load()>=0||give_item.load()>=0||
    max_hearts.load()>=0||set_time.load()>=0||set_day.load()>=0||boss.load()>=0;}
inline constexpr std::array<std::pair<U32,U32>,7> Guards{{
    {0x4bfab4,0xe92d4ff0},{0x4bfabc,0xe1a04000},{0x4bfdbc,0x7752d8},
    {0x4bfdc0,0x626c3c},{0x58d920,0x7752d8},
    {0x7d0910,0xe2804c01},{0x7d08f8,0xebfffa1b}
}};
template<class M> bool CodeMatches(M& m){for(auto [a,w]:Guards)if(m.Read(a)!=w)return false;return true;}
template<class M> std::optional<U32> Get(M& m,U32 at,unsigned bytes) {
    auto w=m.Read(at&~3u);if(!w)return {};
    return (*w>>((at&3)*8))&(bytes==1?255u:65535u);
}
template<class M> void Set(M& m,U32 at,U32 value,unsigned bytes) {
    const U32 shift=(at&3)*8,mask=(bytes==1?255u:65535u)<<shift;
    if(auto word=m.Read(at&~3u))m.Write(at&~3u,(*word&~mask)|((value<<shift)&mask));
}
inline bool Pointer(std::optional<U32> p){return p&&*p>=0x100000&&*p<0x3ffff000&&!(*p&3);}
template<class M> std::optional<U32> Game(M& m) {
    auto context=m.Read(UiContext);
    if(!Pointer(context))return {};
    auto game=m.Read(*context+GameOffset);
    if(!Pointer(game))return {};
    return game;
}
template<class M> bool InGame(M& m) {
    auto game=Game(m);
    if(!game)return false;
    auto state=m.Read(*game+StateOffset);
    auto health=Get(m,MaxHealth,2);
    return state&&(*state&65535)==0x0402&&health&&*health>=16&&*health<=320;
}
template<class M> void FillMagic(M& m) {
    auto unlocked=Get(m,MagicAcquired,1),upgrade=Get(m,DoubleMagicFlag,1);
    if(unlocked&&*unlocked&&upgrade)Set(m,MagicAmount,*upgrade?96:48,1);
}
inline constexpr std::array<U32,4> WalletSizes{99,200,500,500};
template<class M> U32 WalletSize(M& m){return WalletSizes[(m.Read(Upgrades).value_or(0)>>12)&3];}
// Ammo for a newly given item, with a quiver or bomb bag if it needs one.
template<class M> void GiveAmmo(M& m,U32 slot) {
    U32 upgrades=m.Read(Upgrades).value_or(0),amount=0;
    if(slot==1){if(!(upgrades&7))upgrades|=1;amount=30;}
    else if(slot==6||slot==7){if(!((upgrades>>3)&7))upgrades|=1u<<3;amount=20;}
    else if(slot==8)amount=10;
    else if(slot==9)amount=20;
    else if(slot==10||slot==12)amount=1;
    m.Write(Upgrades,upgrades);
    if(amount&&Get(m,AmmoSlots+slot,1).value_or(0)<amount)Set(m,AmmoSlots+slot,amount,1);
}
template<class M> bool GiveItem(M& m,U32 item) {
    U32 slot=0xff;
    if(item<=0x10&&item!=0x05&&item!=0x0b)slot=item;
    else if(item>=0x12&&item<=0x26) {
        // Bottled: an empty bottle first, then a bottle slot you don't have yet.
        for(U32 i=0;i<BottleCount&&slot==0xff;++i)if(Get(m,ItemSlots+BottleSlot+i,1)==0x12u)slot=BottleSlot+i;
        for(U32 i=0;i<BottleCount&&slot==0xff;++i)if(Get(m,ItemSlots+BottleSlot+i,1)==0xffu)slot=BottleSlot+i;
    }
    else if(item>=0x28&&item<=0x2c)slot=5;
    else if(item==0x2d||item==0x2e)slot=58+item-0x2d;
    else if(item==0x2f||item==0x30)slot=17;
    else if(item>=0x32&&item<=0x49)for(U32 i=0;i<MaskSlots.size();++i)if(MaskSlots[i]==item)slot=24+i;
    if(slot==0xff)return false;
    Set(m,ItemSlots+slot,item,1);
    GiveAmmo(m,slot);
    return true;
}
// Equipped sword on every form's B button (Kokiri 0x4d, Razor 0x4e, Gilded 0x4f).
template<class M> void SetButtonSword(M& m,U32 sword) {
    for(U32 i=0;i<20;++i){auto b=Get(m,ButtonItems+i,1);if(b&&*b>=0x4d&&*b<=0x4f)Set(m,ButtonItems+i,sword,1);}
}
template<class M> void Apply(M& m,Action action) {
    switch(action) {
    case FillWallet: case Rupees0:
        Set(m,Rupees,action==FillWallet?WalletSize(m):0,2);Set(m,PendingRupees,0,2);break;
    case Heal:if(auto cap=Get(m,MaxHealth,2))Set(m,Health,*cap,2);break;
    case Magic:FillMagic(m);break;
    case DoubleMagic:
        Set(m,MagicLevel,2,1);Set(m,MagicAcquired,1,1);Set(m,DoubleMagicFlag,1,1);
        Set(m,DoubleDefense,1,1);FillMagic(m);break;
    case Bank5000:case Bank0:Set(m,BankRupees,action==Bank5000?5000:0,2);break;
    case Items:
        for(U32 i:{1u,2u,3u,4u,6u,7u,8u,9u,10u,12u,13u,14u,15u,16u})GiveItem(m,i);
        for(U32 i=0;i<BottleCount;++i)if(Get(m,ItemSlots+BottleSlot+i,1)==0xffu)Set(m,ItemSlots+BottleSlot+i,0x12,1);
        break;
    case Masks:for(U32 i=0;i<MaskSlots.size();++i)Set(m,ItemSlots+24+i,MaskSlots[i],1);break;
    case Gilded:case Razor:
        Set(m,SwordShield,action==Gilded?0x23:0x22,1);
        SetButtonSword(m,action==Gilded?0x4f:0x4e);
        if(action==Razor)Set(m,RazorHits,100,2);
        break;
    case Songs:
        // Four remains and actual MM songs, including the lullaby intro.
        if(auto flags=m.Read(QuestStatus))m.Write(QuestStatus,*flags|0x0101f7cfu);
        break;
    case Fairies:m.Write(StrayFairies,0x0f0f0f0f);break;
    case Bags:
        if(auto upgrades=m.Read(Upgrades))m.Write(Upgrades,(*upgrades&~0x303fu)|27u|0x2000u);
        break;
    case GreatFairySwordB:Set(m,ButtonItems,0x10,1);break;
    case FierceDeityB:Set(m,ButtonItems,0x35,1);break;
    case BomberCode:for(U32 i=0;i<5;++i)Set(m,BomberCodeDigits+i,i+1,1);break;
    case Lottery:for(U32 i=0;i<9;++i)Set(m,LotteryCodes+i,7,1);break;
    default:break;
    }
}
// As the native void respawn: entrance, trigger "start", fade to black.
template<class M> void Warp(M& m,U32 game,U32 entrance,U32 form,U32 time_choice) {
    if(form!=KeepForm){Set(m,SaveForm,form,1);Set(m,SaveMask,MaskFor(form),1);}
    if(time_choice&&time_choice<TimeValues.size()) {
        const U32 time=TimeValues[time_choice];
        Set(m,SaveTime,time,2);
        Set(m,SaveIsNight,time<0x4000||time>=0xc000,1);
    }
    Set(m,game+NextEntrance,entrance,2);
    Set(m,game+TransitionType,2,1);
    Set(m,game+TransitionUnknown,0,1);
    Set(m,game+TransitionTrigger,20,1);
}
// A boss rematch: its temple counts as not cleared while you are in the lair;
// afterwards its flag, remains and heart containers go back to what you had
// unless keep_boss_win.
template<class M> void EndFight(M& m) {
    const Boss& b=*fight.boss;
    if(!keep_boss_win.load()) {
        if(b.week_event>=0){const U32 at=WeekEvents+U32(b.week_event);
            Set(m,at,(Get(m,at,1).value_or(0)&~b.week_mask)|fight.flag_was,1);}
        if(b.remains>=0){const U32 bit=1u<<b.remains;
            if(auto q=m.Read(QuestStatus))m.Write(QuestStatus,(*q&~bit)|fight.quest_was);}
        if(Get(m,MaxHealth,2).value_or(0)>fight.hearts_was) {
            Set(m,MaxHealth,fight.hearts_was,2);
            if(Get(m,Health,2).value_or(0)>fight.hearts_was)Set(m,Health,fight.hearts_was,2);
        }
    }
    fight={};boss_active=false;
}
template<class M> void TickFight(M& m,std::optional<U32> game,bool playing) {
    if(!fight.boss)return;
    ++fight.ticks;
    const auto scene=game&&playing?Get(m,*game+SceneOffset,2):std::nullopt;
    if(scene&&int(*scene)==fight.scene)fight.arrived=true;
    // Left the lair (won, died, warped away), or the warp never happened.
    if((fight.arrived&&scene&&int(*scene)!=fight.scene)||(!fight.arrived&&fight.ticks>200))EndFight(m);
}
template<class M> void StartFight(M& m,U32 game,const Boss& b) {
    fight={};fight.boss=&b;fight.scene=int(b.scene);
    if(b.week_event>=0){const U32 at=WeekEvents+U32(b.week_event),now=Get(m,at,1).value_or(0);
        fight.flag_was=now&b.week_mask;Set(m,at,now&~b.week_mask,1);}
    if(b.remains>=0)fight.quest_was=m.Read(QuestStatus).value_or(0)&(1u<<b.remains);
    fight.hearts_was=Get(m,MaxHealth,2).value_or(0);
    boss_active=true;
    Warp(m,game,b.entrance_group<<9,KeepForm,0);
}
template<class M> void Tick(M& m,bool supported_title) {
    const bool requests=Requested();
    bool supported=supported_title&&CodeMatches(m)&&InGame(m);
    // Preflight every save word used by these cheats before making any writes.
    if(supported)for(U32 at=SaveMask;at<=StrayFairies;at+=4)if(!m.Read(at)){supported=false;break;}
    if(supported)for(U32 at:{BankRupees,WeekEvents,WeekEvents+52,LotteryCodes,LotteryCodes+8,BomberCodeDigits,BomberCodeDigits+4})
        if(!m.Read(at&~3u)){supported=false;break;}
    ready=supported;
    const auto game=supported_title?Game(m):std::nullopt;
    if(supported_title&&fight.boss)TickFight(m,game,supported);
    if(!supported) {
        if(requests){pending=0;warp=-1;give_item=-1;max_hearts=-1;set_time=-1;set_day=-1;boss=-1;result=-1;}
        return;
    }
    int status=requests?1:0;
    const U32 actions=pending.exchange(0);
    for(unsigned i=0;i<Count;++i)if(actions&(1u<<i))Apply(m,Action(i));
    if(const int item=give_item.exchange(-1);item>=0&&!GiveItem(m,U32(item)))status=-2;
    if(const int hearts=max_hearts.exchange(-1);hearts>=3&&hearts<=20) {
        Set(m,MaxHealth,U32(hearts)*16,2);Set(m,Health,U32(hearts)*16,2);
    }
    if(const int time=set_time.exchange(-1);time>=0) {
        Set(m,SaveTime,U32(time),2);Set(m,SaveIsNight,time<0x4000||time>=0xc000,1);frozen_time=U32(time);
    }
    if(const int day=set_day.exchange(-1);day>=1&&day<=3)m.Write(SaveDay,U32(day));
    // Warps wait for any transition under way to finish.
    const bool idle=Get(m,*game+TransitionTrigger,1)==0u;
    if(const int b=boss.load();b>=0&&idle&&!fight.boss) {
        boss=-1;
        if(U32(b)<Bosses.size())StartFight(m,*game,Bosses[b]);
    } else if(const int w=warp.load();w>=0&&idle) {
        warp=-1;
        Warp(m,*game,U32(w)&0xffff,(U32(w)>>16)&15,(U32(w)>>20)&15);
    }
    const U32 held=continuous.load();
    for(unsigned i=0;i<Ammunition.size();++i)if(held&(1u<<i)) {
        const auto& a=Ammunition[i];
        if(Get(m,a.slot,1)==a.item)Set(m,a.count,a.amount,1);
    }
    if(held&(1u<<InfiniteHealth))Apply(m,Heal);
    if(held&(1u<<InfiniteMagic))FillMagic(m);
    if(held&(1u<<InfiniteRupees))Set(m,Rupees,WalletSize(m),2);
    if((held&(1u<<UnbreakableRazor))&&(Get(m,SwordShield,1).value_or(0)&15)==2)Set(m,RazorHits,100,2);
    if(held&(1u<<FreezeTime)) {
        // The clock moves about 17 units between ticks; a bigger jump is the
        // game's own (Song of Time, a new day, a time skip), so follow it.
        const U32 now=Get(m,SaveTime,2).value_or(0),step=(now-frozen_time)&0xffff;
        if(!time_frozen||(step>0x100&&step<0xff00))frozen_time=now;
        time_frozen=true;
        if(now!=frozen_time)Set(m,SaveTime,frozen_time,2);
    } else time_frozen=false;
    if(held&(1u<<SlowTime)){if(m.Read(TimeSpeed)==0u){m.Write(TimeSpeed,0xfffffffeu);slowed=true;}}
    else if(slowed){if(m.Read(TimeSpeed)==0xfffffffeu)m.Write(TimeSpeed,0);slowed=false;}
    if(requests&&result.load()!=status)result=status;
}
/// HID pad update (234 Hz): moon jump takes A from the game while L is held.
inline bool FilterPad(U32& buttons) {
    constexpr U32 A=1u,L=1u<<9;
    if(test_moon_jump.load())buttons|=L|A;
    if(!(continuous.load()&(1u<<MoonJump))||!ready.load()||!(buttons&L))return false;
    const bool jump=buttons&A;
    buttons&=~A;
    return jump;
}
template<class M> void ApplyMoonJump(M& m) {
    const auto game=Game(m);
    if(!game)return;
    const auto player=m.Read(*game+PlayerOffset);
    if(!Pointer(player))return;
    m.Write(*player+PlayerVelocityY,0x41000000u); // 8.0f
    ++test_jump_writes;
}
} // namespace MM3D::SimpleCheats
