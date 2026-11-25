# Native SwiftUI frontend for visionOS (plan)

Status: milestones 1–5 are implemented. The SwiftUI app boots the game in the
visionOS simulator, with the Settings and Controls windows and the in-game
options working there. Build and usage instructions are in `docs/visionos.md`.
Not yet verified on a Vision Pro: device performance with the JIT, real
controller rebinding, background pause and audio interruptions.

The visionOS app currently runs the desktop Qt frontend: a patched, statically
built Qt (`tools/visionos/build-qt.sh`), the full `GMainWindow`, Qt dialogs for
controller mapping, and an in-game ImGui settings overlay that can only be
opened with F1. This plan replaces the Qt *frontend* on visionOS with a native
SwiftUI app. The macOS build keeps Qt.

The work splits into three parts:

1. Move the MM3D/Dekui behaviour out of `citra_qt` into a frontend-neutral
   library, and make the Qt frontend use it (no behaviour change on desktop).
2. An Objective-C++ bridge (`DKEmulator`) that owns the core, the emulation
   thread, the render surface and input, modelled on the Android JNI frontend.
3. The SwiftUI app on top of the bridge.

## 1. MM3D hook extraction

### What lives in the Qt frontend today

| Area | Where | Qt dependency |
|---|---|---|
| JIT26 arena init (must run first, while the debugger is attached) | `citra_qt.cpp:4856` | none |
| Game folder bootstrap: `EnsureRoot`, `Rom`, `Prepare`, `Validate`, `SetUserPath` | `citra_qt.cpp:4899-4917` | QMessageBox for errors |
| Forced settings: single window, no audio stretching | `citra_qt.cpp:1071-1077` | none |
| Forced settings at boot: global resolution/volume/vsync/shader cache/textures, single screen, Vulkan | `citra_qt.cpp:1846-1858` | none |
| ROM re-validation in `BootGame` | `citra_qt.cpp:1769-1781` | QMessageBox |
| Game language apply, every loop and before shutdown | `bootmanager.cpp:117-142, 178-181` | none |
| 16 ms UI poll: controller autoconfig, `PlayerSettings::ApplyPending`, requests (textures folder, controls, shortcut editors, fullscreen, about, quit), frame stepping | `citra_qt.cpp:1266-1403` | dialogs, QDesktopServices |
| Overlay input: F1 toggle, pointer/touch/scroll to `MM3D::Pointer`/`Scroll`, ImGui key and text routing, input latency | `bootmanager.cpp:555-788`, `citra_qt.cpp:1114-1214` | Qt events and key codes |
| Metal layer at contentsScale 1, manual resize | `bootmanager.cpp:445-476, 795-876`, `util/metal_util.mm` | QWindow |
| Startup hint after the loading screen | `citra_qt.cpp:611-622` | signal plumbing only |

Already frontend-neutral and reused unchanged: `game_folder.h`,
`game_setup.h` (visionOS branch), `controller_autoconfig.h`,
`player_settings.h`, `player_settings_bridge.h`, `diagnostics.h`,
`input_latency.h`, `pause_control.h`. They use QtCore at most.

### New library: `dekui_frontend` (`src/dekui_frontend/`)

Plain C++, links `citra_core`, `input_common`, `mm3d_overlay`. QtCore is
allowed (it is already a dependency of `mm3d_overlay`); QtGui/QtWidgets are not.

- `bootstrap.h`: `InitializeJit()`, `PrepareGameFolder()` (returns a
  `SetupError` instead of showing a box), `ValidateBootPath()`, `GamePath()`,
  `LicensesPath()`, `ApplyPlayerDefaults()`, `ApplyBootSettings()`.
- `emu_thread.h`: `GameLanguage`, constructed after load; `ApplyPending()`
  every loop iteration and once before `Shutdown()`.
- `poller.h`: `Poller::Tick()` every ~16 ms on the UI thread (controller
  autoconfig, `PlayerSettings::ApplyPending`, frame stepping) and the `Host`
  interface for the UI the overlay asks for: `ModalOpen`, `PersistConfig`,
  `OpenTextureFolder`, `ShowLicenses`, `ShowAbout`, `ShowControls`,
  `EditShortcut`, `ToggleFullscreenRequested`, `Quit`. `KeyboardDefaults`
  carries the frontend's keyboard codes for fresh profiles.
- `input.h`: neutral `Key`/`Modifiers`, `OverlayToggleKey` (F1),
  `OverlayKeyboard` (text fields in the overlay), `GameKey` (keyboard engine
  plus input-latency tracing), `TouchPointer`. Mouse and scroll go straight to
  `MM3D::Pointer`/`MM3D::Scroll`.

Each Qt frontend site becomes a call into this library: `citra_qt` keeps its
dialogs as the `Host` implementation, maps `Qt::Key` to `Dekui::Key`, and
replaces the poll block, the boot checks and the language lambda. This is
step one because it keeps a single implementation and lets the Mac build
prove nothing changed before any Swift exists.



## 2. Objective-C++ bridge (`src/dekui_visionos/Bridge/`)

Modelled on `src/android/app/src/main/jni/native.cpp`, minus JNI.

### Pieces

- **`EmuWindow_Apple_Vulkan`**: copy of `emu_window_vk.cpp`. `WindowSystemInfo`
  type `MacOS`, `render_surface = (__bridge void*)CAMetalLayer`, scale 1.0. Empty
  `PollEvents`, trivial shared context, no-op `MakeCurrent`/`DoneCurrent`.
  `vk_platform.cpp` already creates the Metal surface from that layer.
- **Config**: the Qt frontend's `configuration/config.cpp` is compiled into
  the bridge with `DEKUI_CONFIG_QTCORE_ONLY` (it needed Qt GUI only for one
  default shortcut), so both apps read and write the same
  `user/config/qt-config.ini`, including input profiles. Keyboard bindings use
  Qt key codes in both apps for the same reason.
- **Input**: keep the SDL engine for controllers (already compiled for
  visionOS, uses GameController underneath, and is what
  `ControllerAutoConfig` and the profile strings expect). Keyboard input from
  `UIPress.key` goes to the `keyboard` engine with HID usage codes, through
  `Dekui::InputRouter`. Controller mapping capture uses
  `InputCommon::Polling` like Qt's `ConfigureInput`.
- **Emulation thread**: a `std::thread` running the Android `RunCitra`
  sequence: load config, `ApplyForcedSettings`, `InputCommon::Init`,
  `system.Load(window, rom)`, `LoadDefaultDiskResources` with progress,
  `RegisterCoreLoopThreadId`, then `RunLoop` with pause/stop atomics and
  `EmuThreadTick`. Shutdown on the same thread.

### Objective-C surface (what Swift sees)

```objc
@protocol DKEmulatorHost <NSObject>          // always called on the main queue
- (void)emulatorFatalError:(NSString*)title message:(NSString*)message;
- (void)emulatorOpenPath:(NSString*)path;
- (void)emulatorShowControls;
- (void)emulatorToggleFullscreen;
- (void)emulatorQuit;
- (void)emulatorLoadProgress:(double)fraction stage:(NSInteger)stage;
- (void)emulatorFirstFrame;
@end

@interface DKEmulator : NSObject
+ (instancetype)shared;
@property (weak) id<DKEmulatorHost> host;

// Lifecycle
- (BOOL)prepareWithDocuments:(NSURL*)root error:(NSError**)error;  // JIT26 + game folder
- (void)startWithLayer:(CAMetalLayer*)layer;
- (void)layoutChanged:(CGSize)points;      // layer frame + UpdateCurrentFramebufferLayout
- (void)pause; - (void)resume; - (void)stop;
@property (readonly) BOOL running, jitEnabled;

// Overlay and input (main thread; forwards to InputRouter)
- (void)toggleDekuiSettings;
- (BOOL)pointerAt:(CGPoint)p button:(NSInteger)b down:(BOOL)down;
- (BOOL)pointerMoved:(CGPoint)p;
- (BOOL)scrollAt:(CGPoint)p delta:(CGVector)d;
- (BOOL)key:(UIKey*)key down:(BOOL)down;

// Core settings: write the PlayerSettings request atomics, read back published values
@property NSInteger resolution, volume, gameLanguage;
@property BOOL vsync, diskShaderCache, hardwareShaders, asyncShaders, customTextures;

// Controller mapping
- (NSArray<DKInputProfile*>*)profiles;
- (NSString*)bindingFor:(DKButton)button;
- (void)captureBindingFor:(DKButton)button completion:(void(^)(NSString* _Nullable))done;
- (void)resetToAutomatic;
@end
```

Threading rule: the bridge never touches `Core::System` from the main thread
except through atomics that already exist for that purpose
(`PlayerSettings::*`, overlay input) or through a small command queue drained
in `EmuThreadTick`. A `CADisplayLink` on the main thread drives
`Dekui::Poller::Tick` (replacing Qt's 16 ms `QTimer`).

## 3. SwiftUI app (`src/dekui_visionos/App/`)

- **Game window** (`WindowGroup`): a `UIViewRepresentable` whose view has
  `layerClass = CAMetalLayer`, contentsScale 1.0, sized to 16:9. UIKit touches
  (pinch, direct touch) go to `pointerAt:`; `pressesBegan/Ended` go to `key:`.
- **Ornament** under the game window: Pause, Dekui settings (toggles the ImGui
  overlay), Settings, Controls.
- **Settings window** (`Window(id: "settings")`): Graphics (resolution, vsync,
  shader cache, custom textures), Audio (volume), Game (language), About
  (licenses, JIT status).
- **Controls window**: connected controllers, 3DS button list with current
  bindings, tap-to-rebind with capture, "Use automatic mapping".
- **Setup view** shown instead of the game when the folder is not ready:
  `fileImporter` to copy the `.cci`/`.3ds` into Documents, and the existing
  "prepare on desktop" message for `ready.json`.
- `scenePhase` → pause/resume.

Dekui presentation settings (HUD, minimap, etc.) stay in the ImGui overlay at
first. Moving them to SwiftUI needs a thread-safe snapshot/apply API on the
overlay's `MM3D::Settings`, which is a later milestone.

## Build

- New CMake option `ENABLE_DEKUI_SWIFTUI` (visionOS only). With it,
  `src/CMakeLists.txt` adds `src/dekui_visionos` instead of `citra_meta`, and
  `mm3d_overlay` links into it. `enable_language(Swift)` with the Xcode
  generator; `SWIFT_OBJC_BRIDGING_HEADER` points at the bridge header.
- Same bundle ID (`org.azahar-emu.azahar`) and Info.plist template as today,
  minus the Qt scene manifest (SwiftUI provides its own). Installing over the
  Qt app keeps the Documents container and the prepared `user/` folder.
- Still needs QtCore for `mm3d_overlay`; `build-qt.sh` can drop Gui, Widgets,
  ShaderTools and Multimedia for this configuration. Removing QtCore from
  `src/mm3d` (27 files) is a separate, optional effort.

## Milestones

1. **Extraction** (done): `dekui_frontend` library; the Qt frontend uses it.
   Verified on the Mac (setup, boot, play).
2. **Skeleton app** (done): bridge, game window, ornament. Boots and renders
   in the simulator. Not yet run on a device.
3. **Settings window** (done): PlayerSettings requests, saved to the shared
   `qt-config.ini` (the Qt config reader compiled against Qt Core only,
   instead of a separate `dekui.ini`).
4. **Controls window** (done): binding display, capture through
   InputCommon pollers or the keyboard, reset to automatic mapping.
5. **Setup and lifecycle** (done): game and user-folder import, setup screen,
   pause in the background. Audio-session interruption handling is SDL's.
6. **Stereoscopic 3D** (done, simulator only so far): RealityKit plane with
   one image per eye, toggled from the ornament or Settings. See
   `docs/visionos.md`.
7. Optional, not started: native SwiftUI versions of Dekui's presentation
   settings (HUD, menus, minimap), which still live in the ImGui overlay;
   removing Qt Core from `src/mm3d`.

## Differences from the plan

- `Window` scenes need visionOS 26; the app uses value-based `WindowGroup`s so
  each auxiliary window has a single instance.
- The bridge is a separate static library: the Swift target must not have
  `src/` on its header path because `src/network/network.h` shadows Apple's
  `Network` module on a case-insensitive file system.
- Qt Core is built without Qt's UIKit entry point (`qt_no_entrypoint`).
- Simulator-only Vulkan workarounds were needed (see `docs/visionos.md`).

## Risks

- CMake's Xcode generator with Swift for visionOS has not been tried in this
  repo; fallback is a checked-in Xcode project that links CMake-built static
  libraries.
- Qt's iOS plugin currently does some setup implicitly (scene, run loop, SDL
  main-ready ordering); each must be redone explicitly.
- QtCore used without a `QCoreApplication`: `QStandardPaths` and
  `QSaveFile` work, but anything relying on the event loop does not. Pass the
  Documents path in rather than relying on `QStandardPaths`.
- The AOT workstream edits CMake files in the same checkout; regenerate the
  project after its changes and keep commits separate.
