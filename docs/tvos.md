# Apple TV (tvOS)

Dekui runs on Apple TV as a native SwiftUI app built from the same bridge as
the visionOS app (`src/dekui_visionos`, Apple TV screens in `TV/`). tvOS never
allows a JIT, so the CPU always runs the DynCom interpreter; build it with
ahead-of-time code ([aot.md](aot.md)) for full speed. The AOT code is AArch64,
which every tvOS 17 Apple TV runs. First tests on an Apple TV 4K (A10X, 2×
internal resolution) ran for minutes with only occasional hitches.

## Build

Qt has no tvOS support; `qtbase-6.10.3-tvos.patch` adds enough of it for Qt
Core, which is all the app uses:

```sh
PROFILE=core TARGET=tvos-device ./tools/visionos/build-qt.sh
```

Configure from the apostrophe-free symlink, from outside the repository, as
for visionOS ([visionos.md](visionos.md)):

```sh
QT=$HOME/Library/Developer/Dekui/qt
SDK_PATH=$(/usr/bin/xcrun --sdk appletvos --show-sdk-path)
BUILD_DIR=$HOME/Library/Developer/Dekui/build/tvos-device-aot

cd /tmp && cmake -S /tmp/mm3d-dekui-visionos-src -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_SYSROOT="$SDK_PATH" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_PREFIX_PATH=$QT/tvos-device-core-6.10.3 \
  -DQt6_DIR=$QT/tvos-device-core-6.10.3/lib/cmake/Qt6 \
  -DQT_HOST_PATH=$QT/host-core-6.10.3 \
  -DENABLE_QT=OFF -DUSE_SYSTEM_QT=ON -DENABLE_DEKUI_SWIFTUI=ON \
  -DENABLE_LIBRETRO=OFF -DENABLE_MM3D_PRESENTATION=ON -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_VULKAN=ON -DENABLE_SOFTWARE_RENDERER=ON -DENABLE_WEB_SERVICE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_SCRIPTING=OFF -DENABLE_ROOM=OFF \
  -DCITRA_USE_PRECOMPILED_HEADERS=OFF -DENABLE_LTO=OFF \
  -DDEKUI_TVOS_CPU=apple-a10 -DDEKUI_AOT_OPTIMIZATION=-O3 \
  -DCITRA_APPLE_DEVELOPMENT_TEAM=<team> \
  -DDEKUI_AOT_GENERATED_DIR=<generated AOT dir> \
  -DDEKUI_VISIONOS_BUNDLE_IDENTIFIER=org.azahar-emu.azahar.aot \
  -DDEKUI_VISIONOS_APP_NAME="Dekui AOT"

cd "$BUILD_DIR" && xcodebuild -project citra.xcodeproj -scheme dekui_visionos \
  -configuration Release -destination id=<Apple TV UDID> \
  -allowProvisioningUpdates -quiet build
xcrun devicectl device install app --device <device> "$BUILD_DIR/bin/Release/Dekui.app"
```

This device build targets the Apple TV 4K's A10X CPU (`apple-a10` is the
matching AppleClang CPU). Set `DEKUI_TVOS_CPU` empty when producing a binary
that must run on the older Apple A7/A8 baseline.

To compare MoltenVK's asynchronous queue processing without changing the CPU
or AOT settings, configure a second build directory with
`-DDEKUI_TVOS_ASYNC_MOLTENVK_SUBMITS=ON`. The default is `OFF`, so the two
variants differ only in MoltenVK queue scheduling.

`xcodebuild -destination` takes the hardware UDID (`devicectl device info
details` lists it); `devicectl` takes the CoreDevice identifier or the name.
Automatic signing uses the team's wildcard tvOS profile. The
increased-memory-limit entitlement in `DekuiTV.entitlements` is dropped by
that profile; the app runs without it.

### Icon

The Home screen icon is a layered (parallax) image stack in
`src/dekui_visionos/TV/Assets.xcassets` ("App Icon & Top Shelf Image"), with
the Top Shelf images beside it. It is drawn from the Azahar logo in
`dist/azahar.svg` (CC BY 4.0), split into four layers: background, the logo's
glow, the petals and the centre, which moves the most when the icon is
focused. After changing the logo or the colors, regenerate the catalog:

```sh
python3 tools/tvos/icon/make_icon.py
```

## The game

tvOS has no Files app, and apps keep their data in `Library/Caches` (the
system may purge it when the Apple TV runs short of storage). Copy the game
dump and a desktop-prepared `user/` folder there:

```sh
tools/tvos/copy-game.sh <device> <folder with the .cci and user/>
```

The script opens Dekui once first: folders that `devicectl` creates belong to
root, so the app must create `Dekui/` and `Dekui/user/` itself or it cannot
write saves, settings and logs. Copy `user/` back the same way (`devicectl
device copy from`) before reinstalling or if storage runs low. A running app
keeps its log open; stop it (`devicectl device process signal --signal
SIGKILL`) before copying `user/log` off the device.

## Controls

- The game reads controllers through SDL (Game Controller framework). The
  Siri Remote is not a game controller.
- **Siri Remote Back or Play/Pause, or both stick clicks (L3+R3)** on a
  controller: opens Dekui's menu, which pauses the game, or closes Game
  Options when they are open. Back (B on a controller) in the menu returns to
  the game. tvOS keeps the controller's Home button for Control Center.
- The menu has Resume, Game Options, Save States, Settings, Controls, Restart
  and Quit. Settings and Controls are saved to `qt-config.ini` like the other
  apps.
- **Game Options** (the in-game ImGui settings) are moved through with the
  D-pad or left stick, A to press, B to back out, L/R to switch tabs or
  windows. The game ignores the controller while they are open.

On first launch the internal resolution drops to 2× if the copied settings ask
for more; an Apple TV GPU cannot keep up with the headset's 4×.

The first boot builds every pipeline in the copied shader cache before the
first frame (about 2.5 minutes on an A10X, black screen). Later boots use the
driver and Metal caches. Pausing (the menu, or leaving the app) saves the
driver pipeline cache.

## Not on Apple TV

Stereoscopic 3D, the immersive view, head tracking, iCloud save-state sync
(tvOS has no iCloud Drive container), microphone input and the in-app game
import.
