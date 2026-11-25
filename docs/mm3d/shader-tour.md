# Shader tour: prebuilt shader cache for Vision Pro and Apple TV

With asynchronous shader compilation there are no hitches, but the first time
an area is drawn its new shaders pop in a moment later. The shader tour plays
through the whole game on a Mac so every shader is recorded in advance; the
headset and the Apple TV then build all of them while the game boots.

## Why a Mac cache works on other devices

Azahar's disk shader cache (`user/shaders/vulkan/transferable/*.vkch`) stores
configs keyed by PICA register state, SPIR-V, and pipeline keys made of shader
ids, fixed-function state and 3DS pixel formats. None of the keys depend on
the GPU. Each file header records the device profile and the shader generator
version; when they differ, the loader rebuilds the SPIR-V from the configs
once and rewrites the file. Measured on 2026-10-30: a Mac cache of 23 VS, 1324
FS and 2462 pipelines loaded in the visionOS simulator with every config
reused and only the 3 pipelines rejected that the Mac rejects too.

Not covered: fragment shaders drawn with the HUD atlas and with custom normal
maps are never written to the disk cache, so they still compile on first use.

Every boot builds every cached pipeline. A full cache makes the first boot on
a device take minutes; later boots are faster thanks to the Metal shader cache.

## Running the tour (macOS)

Requirements: the Dekui macOS app built from this tree (the tour is in
`src/mm3d/shader_tour.h`), with the game set up and file 1 saved.

```sh
python3 tools/mm3d/shader_tour.py
```

Options: `--app` (default `/Applications/Dekui.app` or `Dekui AOT.app`),
`--root` (game folder, default the app's), `--targets visionos,tvos`,
`--seconds` per scene (12), `--form-seconds` (10).

For each target the script:

1. makes a work folder (`~/Library/Application Support/Dekui Shader Tour/<target>`)
   with a clone of the app, a link to the game dump and a copy of `user/`
   without save states or shader caches. Your own saves, save states and
   settings are never written;
2. picks the vertex path of the target. With the JIT, 60 fps interpolation
   moves vertices on the CPU and hardware vertex shaders are off (`visionos`:
   Vision Pro with JIT). Without the JIT it runs on the GPU with hardware
   vertex shaders on (`tvos`: Apple TV, and Vision Pro running Dekui AOT).
   The two give different vertex shaders and pipelines. Also turns on
   stereo 3D (side by side), custom textures, async shaders and the disk
   cache, and runs the game in a window rather than fullscreen;
3. starts the game with `DEKUI_SHADER_TOUR=1`. The game presses A through the
   title screen and loads file 1, then warps to every entrance group
   0x00-0x6F except 0x0B, 0x0C, 0x0D, 0x0F, 0x2E and 0x37 (they load no
   scene) and 0x1D (the game freezes there), then to 15 main areas as Deku,
   Goron, Zora and Fierce Deity Link and the same areas at midnight. In each scene the input walks, attacks, rolls or
   jumps, charges a spin attack and turns. Health is kept full and the day is
   held before the moon falls. A crash or stall restarts the game at the next
   step;
4. adds what the game folder's own cache (your play on this Mac) has that the
   tour did not draw (skip with `--no-play-cache`; for visionOS without its
   hardware vertex shaders), saves the result in
   `~/Library/Application Support/Dekui Shader Cache/<target>` and prints the
   entry counts.

Coverage, measured against a Mac play cache: the tour alone draws 57% of the
fragment shaders that play used, plus about 1700 that play had not reached.
Walking through scenes misses item effects, enemies being hit, menus and
cutscenes; that is why your own play cache is added.

It takes about 55 minutes per target. The game window opens in the background.

## Getting the cache onto the device

The device merges any `*.vkch` files it finds in `user/shaders/import/` into its
own cache at boot (`ShaderDiskCache::ImportCaches`), keeping its own entries,
then deletes the imported copies. The merged file is marked so that the first
boot regenerates the SPIR-V for that device.

- **Apple Vision Pro**: the script copies the visionOS cache (or with
  `--icloud-target tvos`, the no-JIT one for Dekui AOT) to iCloud Drive,
  `Dekui/Shader Cache`. On launch the app copies new versions from there to the
  import folder (`Dekui::CloudShaderCache::Fetch`). If iCloud is off, copy the
  four files into `Documents/user/shaders/import/` with the Files app or Finder.
- **Apple TV** (no iCloud Drive): copy the tvOS cache with `devicectl`; the
  script prints the command. The game folder is `Library/Caches/Dekui`.

Users build the cache from their own game; no game data is in this repository.
