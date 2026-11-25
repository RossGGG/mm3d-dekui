# visionOS port status

There are two visionOS frontends:

- **The native SwiftUI app** (`src/dekui_visionos`, `-DENABLE_DEKUI_SWIFTUI=ON`):
  visionOS windows and ornaments, a Settings window, a controller mapping
  window, and an in-app setup screen. It needs only Qt Core. See
  [Native SwiftUI app](#native-swiftui-app) below and `docs/visionos-swiftui.md`.
- **The Qt frontend** (`citra_meta`), described in the rest of this file.

The native DekUI application builds, installs, and runs on a physical Apple
Vision Pro. Launched through StikDebug it is fully playable with the Dynarmic
CPU JIT and the shader JIT, and audio plays through SDL2's CoreAudio backend.
It also builds and launches in the Apple Vision Pro simulator (interpreters
only). The libretro core builds for both the simulator and physical devices.

## Verified toolchain

- Xcode 26.3
- visionOS 26.2 SDK
- CMake 4.4.4
- Qt 6.10.3, built from source
- arm64
- minimum deployment target: visionOS 2.0

Use a source path without apostrophes. Some generated Xcode shell phases do not
quote such paths correctly. A symlink is sufficient:

```sh
ln -s "$PWD" /tmp/mm3d-dekui-visionos-src
```

## Build Qt for the simulator

Qt's macOS binaries cannot be reused on visionOS. The helper below builds a
matching host Qt and a static visionOS-simulator Qt containing Core, Gui,
Widgets, ShaderTools, and Multimedia:

```sh
./tools/visionos/build-qt.sh
```

The default installations are:

- host tools: `~/Library/Developer/Dekui/qt/host-full-6.10.3`
- visionOS simulator libraries: `~/Library/Developer/Dekui/qt/visionos-sim-full-6.10.3`

Do not install Qt under `/tmp`: macOS deletes `/tmp` files that have not been
used for three days, which leaves a broken Qt that CMake can no longer find.

The helper applies the two patches in `tools/visionos/` that Qt 6.10.3 needs
with Xcode 26: simulator Swift target/include handling and explicit Objective-C
framework imports in Qt Multimedia. It also normalizes Xcode 26's
`Release-xrsimulator` object directories for Qt's installer.

The locations can be changed with `QT_ROOT`, `WORK_ROOT`, `HOST_PREFIX`,
`TARGET_PREFIX`, and `PARALLEL` environment variables. `PROFILE=core` builds
only Qt Core, which is all the SwiftUI app needs, in a fraction of the time.

## Build Qt for a physical Vision Pro

The device libraries are a separate build because visionOS simulator and
visionOS device binaries use different Apple platforms. Build them with:

```sh
TARGET=device ./tools/visionos/build-qt.sh
```

The default device installation is
`~/Library/Developer/Dekui/qt/visionos-device-full-6.10.3`.

## Build and run native DekUI

```sh
SDK_PATH=$(/usr/bin/xcrun --sdk xrsimulator --show-sdk-path)
BUILD_DIR=/private/tmp/mm3d-dekui-visionos-ui-build

cmake -S /tmp/mm3d-dekui-visionos-src -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_SYSTEM_NAME=visionOS \
  -DCMAKE_OSX_SYSROOT="$SDK_PATH" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 \
  -DCMAKE_PREFIX_PATH=$HOME/Library/Developer/Dekui/qt/visionos-sim-full-6.10.3 \
  -DQT_HOST_PATH=$HOME/Library/Developer/Dekui/qt/host-full-6.10.3 \
  -DENABLE_LIBRETRO=OFF \
  -DENABLE_QT=ON \
  -DUSE_SYSTEM_QT=ON \
  -DENABLE_QT_TRANSLATION=OFF \
  -DENABLE_MM3D_PRESENTATION=ON \
  -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_VULKAN=ON \
  -DENABLE_SOFTWARE_RENDERER=ON \
  -DENABLE_WEB_SERVICE=OFF \
  -DENABLE_DISCORD_RPC=OFF \
  -DENABLE_SCRIPTING=OFF \
  -DENABLE_ROOM=OFF \
  -DCITRA_USE_PRECOMPILED_HEADERS=OFF \
  -DENABLE_LTO=OFF

cmake --build "$BUILD_DIR" --config Release --target citra_meta \
  --parallel 8 -- -quiet CODE_SIGNING_ALLOWED=NO
```

The unsigned simulator application is
`$BUILD_DIR/bin/Release/azahar.app`. Install and launch it with:

```sh
DEVICE_ID=$(/usr/bin/xcrun simctl list devices available |
  sed -n 's/.*Apple Vision Pro (\([^)]*\)).*/\1/p' | head -1)

/usr/bin/xcrun simctl boot "$DEVICE_ID" 2>/dev/null || true
/usr/bin/xcrun simctl bootstatus "$DEVICE_ID" -b
/usr/bin/xcrun simctl install "$DEVICE_ID" \
  "$BUILD_DIR/bin/Release/azahar.app"
/usr/bin/xcrun simctl launch "$DEVICE_ID" org.azahar-emu.azahar
```

## Build and run on Vision Pro

Pair the headset in Xcode's Devices and Simulators window and enable Developer
Mode on the Vision Pro. Generate a device Xcode project with:

```sh
SDK_PATH=$(/usr/bin/xcrun --sdk xros --show-sdk-path)
BUILD_DIR=/private/tmp/mm3d-dekui-visionos-device-build

cmake -S /tmp/mm3d-dekui-visionos-src -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_SYSTEM_NAME=visionOS \
  -DCMAKE_OSX_SYSROOT="$SDK_PATH" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 \
  -DCMAKE_PREFIX_PATH=$HOME/Library/Developer/Dekui/qt/visionos-device-full-6.10.3 \
  -DQT_HOST_PATH=$HOME/Library/Developer/Dekui/qt/host-full-6.10.3 \
  -DENABLE_LIBRETRO=OFF \
  -DENABLE_QT=ON -DUSE_SYSTEM_QT=ON \
  -DENABLE_QT_TRANSLATION=OFF \
  -DENABLE_MM3D_PRESENTATION=ON \
  -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_VULKAN=ON -DENABLE_SOFTWARE_RENDERER=ON \
  -DENABLE_WEB_SERVICE=OFF -DENABLE_DISCORD_RPC=OFF \
  -DENABLE_SCRIPTING=OFF -DENABLE_ROOM=OFF \
  -DCITRA_USE_PRECOMPILED_HEADERS=OFF -DENABLE_LTO=OFF
```

Open `$BUILD_DIR/citra.xcodeproj`, select the `citra_meta` scheme and your
Vision Pro destination, then choose your Apple development team under Signing
& Capabilities. Xcode will automatically manage the development certificate
and provisioning profile. Build and Run installs and launches the app on the
headset. To preserve the team in regenerated projects, pass its 10-character
ID as `-DCITRA_APPLE_DEVELOPMENT_TEAM=TEAMID` when configuring.

### JIT

Vision Pro enforces TXM: the app can never make its own memory executable
(`MAP_JIT` is rejected and `mprotect` silently drops `PROT_EXEC`). Only memory
that an attached debugger allocated and wrote to can run code. Dekui uses the
StikDebug "JIT26" protocol (`brk #0xf00d`) to ask the debugger for one
executable arena at startup, then writes code through a local writable alias of
it. Dynarmic and the shader JIT both allocate from that arena (see
`externals/oaknut/include/oaknut/apple_jit26.hpp`). On startup the app logs
`[JIT26] CPU backend: Dynarmic JIT` or `... DynCom interpreter`.

If no debugger is attached at launch, the app logs `[JIT26] no debugger
attached` and falls back to the CPU and shader interpreters. A home-screen
launch therefore runs on DynCom: close to full speed, but audio still skips
occasionally.

- **StikDebug (recommended):** launch Dekui from StikDebug with its universal
  JIT script. This is verified on device and is the path to use for play.
  Apparition-style enablers that implement the same protocol should also work.
- **Xcode Build & Run:** add this line to `~/.lldbinit-Xcode` so LLDB answers the
  JIT request:

  ```
  command script import /path/to/repo/tools/visionos/jit26_lldb.py
  ```

  The debugger must write every 16 KiB page of the arena over Wi-Fi (around
  five minutes for 96 MiB), so the generated scheme sets
  `DEKUI_JIT26_ARENA_MB` to 96 (configure with
  `-DCITRA_VISIONOS_JIT26_ARENA_MB=<MiB>` to change it). Without it the app asks
  for 128 MiB. Use this path for debugging, not for play.
- Set `DEKUI_JIT26_NO_DETACH=1` to keep the debugger attached after the arena
  has been prepared, for an enabler that revokes execution on detach.

The arena is a fixed budget, so the JIT is careful with it:

- Each Dynarmic instance uses a 16 MiB code cache. New 3DS mode runs four
  emulated cores, so 64 MiB is in use at once.
- Each core keeps only the JIT for its current page table. When a process
  switch moves a core to a new page table, the JITs for other page tables are
  freed before the new one is allocated (deferred to the next `Run` if the
  switch happens inside an SVC). Without this, stale per-core JITs exhaust the
  arena.
- If the shader JIT cannot get executable memory, that shader falls back to
  the shader interpreter instead of asserting.

### Submodule patches

The JIT and audio work needs small changes inside three submodules. They are
kept as patches in `tools/visionos/` and applied by `externals/CMakeLists.txt`
at configure time when `CMAKE_SYSTEM_NAME=visionOS`:

- `oaknut-visionos-jit26.patch`: `apple_jit26.hpp` and the `CodeBlock`
  executable/writable alias (`wptr()`).
- `dynarmic-visionos-jit26.patch`: arm64 backend writes code through
  `wptr()`; a plain atomic spin lock replaces the JIT-emitted one.
- `sdl2-visionos-coreaudio.patch`: treats visionOS like iOS so SDL uses
  AVAudioSession instead of the macOS CoreAudio device APIs.

Each patch is skipped if it is already applied and configuration fails if it
no longer applies cleanly (for example after a submodule update). The patches
change nothing outside visionOS, so a checkout that has them applied still
builds for desktop.

### Game data and the prepared user folder

On first launch, open Files > On My Apple Vision Pro > Dekui and import one
decrypted USA v1.1 game-card dump (`.cci` or `.3ds`) there. `Dekui` is the
app's Files-visible Documents container; there is no additional Documents or
Dekui subfolder inside it. The existing Python/QProcess setup helper cannot run
in a visionOS sandbox, so `user/setup/ready.json` and any prepared assets must
come from the desktop preparation workflow.

Deleting the app deletes its container, including that `user/` folder. Keep a
copy on the Mac and restore it after a reinstall with:

```sh
DEVICE=<CoreDevice identifier from `xcrun devicectl list devices`>

xcrun devicectl device copy to --device "$DEVICE" \
  --domain-type appDataContainer \
  --domain-identifier org.azahar-emu.azahar \
  --source /path/to/prepared/user \
  --destination Documents/user
```

Install the app once (Xcode, or `xcrun devicectl device install app --device
"$DEVICE" azahar.app`) before copying, so that its container exists.
`devicectl` only accepts the CoreDevice identifier; `xcodebuild -destination`
needs the hardware UDID instead.

## Native SwiftUI app

Build Qt Core once per platform (a few minutes each):

```sh
PROFILE=core TARGET=simulator ./tools/visionos/build-qt.sh
PROFILE=core TARGET=device ./tools/visionos/build-qt.sh
```

Configure from the apostrophe-free symlink, and run CMake from a directory
outside the repository, otherwise the source path resolves to the real one and
Xcode's generated script phases fail:

```sh
QT=$HOME/Library/Developer/Dekui/qt
SDK_PATH=$(/usr/bin/xcrun --sdk xrsimulator --show-sdk-path)
BUILD_DIR=/private/tmp/mm3d-dekui-swiftui-sim-build

cd /tmp && cmake -S /tmp/mm3d-dekui-visionos-src -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT="$SDK_PATH" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 \
  -DCMAKE_PREFIX_PATH=$QT/visionos-sim-core-6.10.3 \
  -DQt6_DIR=$QT/visionos-sim-core-6.10.3/lib/cmake/Qt6 \
  -DQT_HOST_PATH=$QT/host-core-6.10.3 \
  -DENABLE_QT=OFF -DUSE_SYSTEM_QT=ON -DENABLE_DEKUI_SWIFTUI=ON \
  -DENABLE_LIBRETRO=OFF -DENABLE_MM3D_PRESENTATION=ON -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_VULKAN=ON -DENABLE_SOFTWARE_RENDERER=ON -DENABLE_WEB_SERVICE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_SCRIPTING=OFF -DENABLE_ROOM=OFF \
  -DCITRA_USE_PRECOMPILED_HEADERS=OFF -DENABLE_LTO=OFF

cmake --build "$BUILD_DIR" --config Release --target dekui_visionos \
  --parallel 8 -- -quiet CODE_SIGNING_ALLOWED=NO
```

The app is `$BUILD_DIR/bin/Release/Dekui.app`. For a device build use the
`xros` SDK and the `visionos-device-core-6.10.3` Qt, then open
`$BUILD_DIR/citra.xcodeproj`, pick the `dekui_visionos` scheme and your team,
and Build & Run, or build with `xcodebuild` and install with `devicectl` as
for the Qt app. JIT works the same way (StikDebug, or the Xcode LLDB script).

The app has the same bundle identifier as the Qt app, so installing it over
the Qt app keeps the Documents folder with the game and the prepared `user/`
folder. Settings are read from and saved to the same
`user/config/qt-config.ini`.

On launch the app checks the Dekui folder. Without a game or a prepared user
folder it shows a setup screen that can import both from Files. Once the game
is running:

- the ornament under the game window has Pause, Game Options (Dekui's
  in-game settings overlay), 3D, Restart (reboots the game after a
  confirmation), Controls, Settings and Quit;
- **Settings** covers resolution, V-Sync, custom textures, shader options,
  stereoscopic 3D and its depth, volume and game language, and saves
  immediately;
- **Controls** shows the connected controllers and the current bindings; tap a
  control and press a button, key or stick to rebind it, or return to the
  automatic mapping;
- a hardware keyboard drives the game with the same bindings as the desktop.
- controller buttons go to the game, not to the UI element you look at: the
  game view has a `GCEventInteraction` for gamepad events. Do not set
  `GCSupportsControllerUserInteraction`; it hands the controller to the UI.

Launch arguments for screenshots and UI tests: `-DekuiOpenPanel settings`
(or `controls`, `licenses`) opens that window once setup succeeds, and
`-DekuiShowGameOptions YES` opens the in-game options once the game is
running.

### Motion controls

`Settings::values.motion_source` (Off / Controller / Head tracking, saved in
`qt-config.ini` under `[Controls]` with `motion_sensitivity`) chooses what the
3DS gyroscope and accelerometer report. `src/dekui_frontend/motion.cpp`
registers the `dekui_motion` input engine, and HID uses it in place of the
profile's `motion_device` when it is registered (Dekui frontends only):

- Controller: the gyroscope of the controller the profile is bound to (else
  the last one used), read through SDL: HIDAPI on macOS, `GCMotion` through
  SDL's MFi driver on visionOS. Without a gyro controller, the profile's own
  motion device is used (mouse tilt on the desktop). Controllers that report
  no accelerometer (DualSense and DualShock 4 through GameController) read a
  level console for it.
- Head tracking (visionOS): ARKit's `WorldTrackingProvider` device anchor,
  converted to angular velocity and gravity in `Dekui::Motion::SetHeadPose`.
  ARKit only reports while an immersive space is open, so Dekui keeps its
  dim-surroundings space open (without dimming unless asked). Recenter makes
  the current head pose the level 3DS.

Axes follow InputCommon's SDL mapping: the controller or head counts as a 3DS
lying flat in front of the player, so turning left is a turn about 3DS y+.

### Stereoscopic 3D

With 3D on, the game renders both eyes, as on a 3DS with the 3D slider up, and
each of the headset's eyes sees its own image. A `CAMetalLayer` shows one image
to both eyes, so stereo frames take a different path:

1. `render_3d` is `SideBySideFull` and `factor_3d` is the depth (both in
   `qt-config.ini`). The game view's size is doubled in width
   (`EmuWindow_Apple_Vulkan::Resize`), so each eye gets the whole view.
2. The Dekui overlay is laid out for one eye and drawn into both halves
   (`MM3D::Overlay::Draw`, `ImGui_ImplVulkan_SetViewportOffsetX`).
3. The presentation thread hands the finished frame to
   `Vulkan::ExternalPresent` instead of the swapchain; the frame images are
   created exportable with `VK_EXT_metal_objects`.
4. `StereoFrames` (bridge) copies it into one of three sRGB Metal textures.
5. On the main thread `StereoScreen` (Swift) copies the newest one into a
   RealityKit `LowLevelTexture` on a plane on the window's surface. A
   RealityView in a window has its origin at the center of a deep frame, so
   the plane is placed on the back face of the view's converted frame,
   not at the origin. Its
   shader graph material uses RealityKit's camera index switch to sample the
   left half for the left eye and the right half for the right eye. Touches
   still go to the game view underneath.

Mono captures (screenshots, the simulator) show the left eye.

Dekui's overlays that stand in for the lower screen (notebook copy, minimap,
presentation profiles) are drawn into both eyes at the same place, so they
sit at screen depth (`RendererVulkan::DrawScreens`). Overlays captured from the
top screen are captured from the left eye only, since the right eye's draws
would otherwise be copied into the same layer a second time.

Widescreen works per eye: each eye's top screen is fitted to its half
(`RendererVulkan::DrawScreens`). In 2D the game renders the top screen at
480x400; in 3D each eye is 240x400. Draw signatures that were recorded in 2D
(the widescreen fade exemption in `WidescreenProbe`, transition fades) compare
the width through `Widescreen::SignatureWidth`, so they match in 3D too.
Otherwise full-screen fades get squeezed and leave white bars at the edges.
The bloom fix's final composite pass is likewise accepted at both sizes
(`BloomSampling::Matches`). The 60 Hz camera presentation also works in
3D. MM3D draws both eyes into the same render target, left first, and copies
each to its display buffer. The rasterizer therefore keeps a second copy of the
midpoint state (camera history, private layers, per-frame bookkeeping) and
swaps it in when the left eye is copied out (`SetMidpointEye`), swapping back
at the right eye's copy and at the end of the frame. Both eyes use the same
camera; only the projection differs. A pair is presented only when both eyes
have a midpoint frame, and `DrawMidpointTop` draws each eye's into its half.
The eye switch follows the game's copies to the display buffers, not the 3D
setting: after 3D is turned off MM3D keeps drawing a right eye, and without
the switch both eyes would be mixed into the 2D midpoint frame (a doubled
image). With 3D off the game is told the 3D slider is down
(`Settings::Effective3DSlider`), so it draws both eyes from the same place.

The simulator lacks two GPU features every Vision Pro has. Simulator builds
work around them in `vk_simulator_workarounds.h` (texel buffers in private
memory, rebased indices) and in the ImGui Vulkan backend (per-list vertex
binding). Device builds are unaffected.

### Memory and HD texture packs

Dekui runs under a per-app memory limit (about 5.1 GB on a Vision Pro by
default), and Metal allocations count against it. A 4K texture costs 64 MiB
on the GPU. The custom texture loader used to keep each decoded texture in
memory as well, which doubled that, so loading an area with a 4K pack (fast
with the JIT) exceeded the limit and the system killed the app without a
crash report. Now the decoded pixels are freed after the GPU upload
(`Material::Unload`, called from `CustomTexManager::TickFrame`), except when
textures are preloaded.

The app requests the increased memory limit and iCloud (`Dekui.entitlements`).
Those entitlements need an explicit App ID, `UJYC8HY4F2.org.azahar-emu.azahar`;
builds before 2026-10-20 used the team's wildcard profile (`U8RU5NJ96L.*`).
visionOS refuses to upgrade across an App ID prefix change
(MismatchedApplicationIdentifierEntitlement), so moving an install from one to
the other means deleting the app, which wipes Documents: back it up with
`devicectl device copy from` and restore it with `copy to` afterwards.

Save states sync through iCloud Drive (`Bridge/icloud_states.mm`). When the
signed-in account has iCloud Drive, `UserPath::StatesDir` points at
`Documents/States` in the app's iCloud container. Existing local states are
copied there at launch (the newer copy wins; the local folder is left as a
backup). States from other devices are downloaded at launch, every 30 s, and
when Save States opens. Without iCloud Drive, states stay in `user/states`.

Save states used to build the whole serialized system (several hundred MB)
in a string stream, copy it, then compress it, adding about 700 MB at once,
which killed the app with a 4K pack loaded. They now compress while
serializing and decompress while deserializing
(`Common::Compression::ZSTDCompressingBuffer` / `ZSTDDecompressingBuffer`):
saving adds about 20 MB and loading about 80 MB. Old save files still load.
New files are a zstd frame without a content size, so older builds can't load
them (save states are build-specific anyway).

For diagnosing kills on the headset, the app keeps:
- `user/log/history/`: the last 8 logs, since the automatic relaunch after a
  kill rotates the crashed run's log away;
- `user/log/crash-<time>.txt`: the faulting thread's stack on a fatal signal;
- `user/log/memory.txt` (previous run: `memory.old.txt`): footprint, memory
  still available and debugger state, once a second, unbuffered.

Pull them with
`xcrun devicectl device copy from --device <id> --domain-type appDataContainer
--domain-identifier org.azahar-emu.azahar --source Documents/user/log
--destination <dir>`.

## Core-only smoke builds

For a simulator core, configure with `-DENABLE_LIBRETRO=ON`,
`-DCMAKE_OSX_SYSROOT=xrsimulator`, `-DENABLE_VULKAN=OFF`, and build the
`citra_libretro` target. For a device core, use `xros` and append
`CODE_SIGNING_ALLOWED=NO` to the Xcode build. Both variants have been verified
as arm64 Mach-O binaries with the correct visionOS platform load command.

## Known limitations

- Audio output goes through SDL2's CoreAudio (AVAudioSession) backend because
  OpenAL Soft's CoreAudio backend needs macOS-only headers. The SDL sink calls
  `SDL_SetMainReady()` (Qt, not SDL, owns `main`) and uses a 2048-sample
  buffer. Microphone input is disabled.
- SDL's unused UIKit video, URL launcher, and raw HID paths are disabled.
  GameController-based input remains compiled but still needs runtime testing.
- Camera capture is not provided by Qt Multimedia on visionOS.
- Without a JIT enabler the DynCom interpreter fallback is near full speed
  but audio skips occasionally. The GDB stub is compiled out on visionOS to
  keep the interpreter's per-instruction checks cheap.
- The simulator still runs the interpreters.
- Injected taps from simulator automation tools do not reach visionOS app
  content; use the launch arguments above to open windows for screenshots.
- Controller input, suspend/resume, and saves remain to be tested on hardware.
