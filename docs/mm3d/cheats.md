# Cheats

Game Options (F2 on macOS, the Game Options button on Vision Pro and Apple TV)
has a **Cheats** tab. It is part of the in-game overlay
(`src/mm3d/overlay.cpp`), so every build that shows Game Options has it. The
cheats themselves are in `src/mm3d/simple_cheats.h`, run from the cheat
engine's periodic callback (`src/core/cheats/cheats.cpp`, about 5 times a
second of emulated time) and, for moon jump, from the HID pad update.

Requests made with the overlay open are queued and apply when the game
resumes. They need a loaded save in the supported USA Restoration build; code
guards in `SimpleCheats::Guards` check that before any write. Infinite and
continuous cheats last until the game quits.

| Section | Cheats |
| --- | --- |
| Warp | Any of 99 areas (the entrance groups the shader tour visits, by region), arriving as the current form, Human, Deku, Goron, Zora or Fierce Deity, at the current time, 6 AM, noon, 6 PM or midnight. Uses the native void-respawn transition, as the shader tour does. |
| Boss rematch | Odolwa, Goht, Gyorg, Twinmold, Igos du Ikana, Majora. The temple counts as not cleared while you are in the lair; its flag, remains and heart containers are restored when you leave unless "Keep the win" is on (as MM3D-PC). |
| Health, magic and rupees | Refill hearts or magic, set 3–20 heart containers, double magic + enhanced defense, fill wallet, 0 rupees, bank 5000 / 0. |
| Inventory and gear | All items (with ammo; bottle contents kept), all masks, give any single item, Gilded or Razor Sword + Mirror Shield, remains and songs, stray fairies, largest quiver/bomb bag/wallet, Great Fairy's Sword or Fierce Deity's Mask on B. |
| Time | Set 6 AM, noon, 6 PM or midnight; day 1, 2 or final day. |
| Minigames | Bombers' code 12345, lottery 777. |
| Infinite and continuous | Each ammo type, health, magic, rupees, unbreakable Razor Sword, freeze time, slow time, moon jump (hold L + A). |

Sources: the CTRPF code list for MM3D USA (save-data codes, addresses less
0x1000 for this build) and MM3D-PC's F1 > Cheats (warp, boss rematch, freeze
time, moon jump, give item). CTRPF's code patches (easy minigames, use items
anywhere, burning Deku sticks) target vanilla `.text`, which Restoration
relinks, so they are not included. MM3D-PC's fly and actor spawn need hooks
inside the game frame and are not included either.

The mask order comes from the game's slot table at 0x6a0b48 (the CTRPF
"all masks" code puts masks in the wrong slots), and MM3D has six bottle
slots (48–53).

## Self-test

`DEKUI_CHEAT_TEST=1` (`src/mm3d/cheat_test.h`) runs the cheats through the
same requests as the panel once a save is in play and logs `Cheat test: PASS`
/ `FAIL` lines to `azahar_log.txt`. Run it on a copy of the user folder, with
the shader tour switches to load file 1 hands-free:

```sh
open -n -g --env DEKUI_SHADER_TOUR=1 --env DEKUI_SHADER_TOUR_START=999 \
  --env DEKUI_CHEAT_TEST=1 "<work folder>/Dekui.app"
```

2026-10-31 on macOS: 0 failures (one-shot cheats, give item, freeze time,
warp to Woodfall as Deku at midnight, Odolwa rematch and its restore, moon
jump).
