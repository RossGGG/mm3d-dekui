// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Emulator settings. Changes are applied on the next frame and saved to
/// user/config/qt-config.ini, shared with the desktop app.
struct SettingsView: View {
    @Environment(EmulatorModel.self) private var model
    @Environment(\.openWindow) private var openWindow

    @State private var resolution = 4
    @State private var volume = 100.0
    @State private var vsync = true
    @State private var diskShaderCache = true
    @State private var hardwareShaders = false
    @State private var asyncShaders = false
    @State private var customTextures = false
    @State private var language = 1
    @State private var stereoscopic = false
    @State private var depth3D = 100.0
    @State private var motionSensitivity = 100.0
    @State private var allowJIT = true
    @State private var loaded = false

    private var emulator: DKEmulator { model.emulator }

    private var gameStarted: Bool { model.state == .running || model.state == .paused }

    /// The running game's CPU backend; the JIT needs an enabler at launch.
    private var cpuStatus: String {
        guard gameStarted else { return "Game not started" }
        let backend = emulator.cpuBackend
        if allowJIT && !emulator.jitEnabled && backend != "JIT" {
            return "\(backend) (no JIT enabler)"
        }
        return backend
    }

    /// The setting differs from what the running game uses.
    private var needsRestart: Bool {
        guard gameStarted else { return false }
        let usingJIT = emulator.cpuBackend == "JIT"
        return (allowJIT && emulator.jitEnabled && !usingJIT) || (!allowJIT && usingJIT)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Graphics") {
                    Picker("Internal resolution", selection: $resolution) {
                        ForEach(1...9, id: \.self) { scale in
                            Text("\(scale)× (\(400 * scale)×\(240 * scale))").tag(scale)
                        }
                    }
                    Toggle("V-Sync", isOn: $vsync)
                    Toggle("Custom textures", isOn: $customTextures)
                }
                Section {
                    Toggle("Disk shader cache", isOn: $diskShaderCache)
                    Toggle("Hardware shaders", isOn: $hardwareShaders)
                    Toggle("Asynchronous shader compilation", isOn: $asyncShaders)
                } header: {
                    Text("Shaders")
                } footer: {
                    Text("Shader changes take effect the next time the game starts.")
                }
                Section {
                    Toggle("Stereoscopic 3D", isOn: $stereoscopic)
                    LabeledContent("Depth") {
                        Slider(value: $depth3D, in: 0...100, step: 1)
                    }
                    .disabled(!stereoscopic)
                } header: {
                    Text("3D")
                } footer: {
                    Text("Shows the game's 3D effect, with one image for each eye.")
                }
                Section {
                    Toggle("Immersive view", isOn: Binding(
                        get: { model.wideView }, set: { model.wideView = $0 }))
                    LabeledContent("Field of view") {
                        HStack {
                            Slider(value: Binding(
                                get: { Double(model.wideViewFov) },
                                set: { model.wideViewFov = Int($0) }), in: 50...120, step: 5)
                            Text("\(model.wideViewFov)°")
                                .monospacedDigit()
                                .frame(minWidth: 44, alignment: .trailing)
                        }
                    }
                    LabeledContent("Keep character centred") {
                        HStack {
                            Slider(value: Binding(
                                get: { Double(model.wideViewCentring) },
                                set: { model.wideViewCentring = Int($0) }), in: 0...100, step: 10)
                            Text("\(model.wideViewCentring)%")
                                .monospacedDigit()
                                .frame(minWidth: 44, alignment: .trailing)
                        }
                    }
                    Picker("Final texture scale", selection: Binding(
                        get: { model.immersiveUpscaling },
                        set: { model.immersiveUpscaling = $0 })) {
                        ForEach(ImmersiveUpscaling.allCases) { mode in
                            Text(mode.title).tag(mode)
                        }
                    }
                    Toggle("Anti-aliasing", isOn: Binding(
                        get: { model.immersiveAntialiasing },
                        set: { model.immersiveAntialiasing = $0 }))
                    .disabled(model.immersiveUpscaling == .off)
                } header: {
                    Text("Immersive view (experimental)")
                } footer: {
                    Text("Shows the game on a large curved screen around you with a wider field "
                        + "of view. Turning your head turns the game's view in place, and the "
                        + "screen stays fixed in the room between game frames. Combine with "
                        + "head-tracking motion controls to aim by looking in first person. The "
                        + "field of view is vertical; the standard 3DS view is 45°. Keep character "
                        + "centred tilts the game world toward keeping your character where you "
                        + "looked at Recenter; lower keeps the game's horizon level with the room. "
                        + "The tilt follows the camera slowly, so jumps don't move the world. Final texture "
                        + "scale uses GPU upscaling after rendering; anti-aliasing smooths the "
                        + "scene without softening the separate HUD.")
                }
                Section {
                    Toggle("Auto-hide game controls", isOn: Binding(
                        get: { model.autoHideOrnament },
                        set: { model.autoHideOrnament = $0 }))
                } header: {
                    Text("Window")
                } footer: {
                    Text("Hides the controls below the game window while you play. Pinch on the game to show them again. They always hide in full screen.")
                }
                Section {
                    Picker("Motion controls", selection: Binding(
                        get: { model.motionSource },
                        set: { model.motionSource = $0 })) {
                        Text("Off").tag(DKMotionSource.off)
                        Text("Controller gyro").tag(DKMotionSource.controller)
                        Text("Head tracking").tag(DKMotionSource.headTracking)
                    }
                    LabeledContent("Sensitivity") {
                        HStack {
                            Slider(value: $motionSensitivity, in: 25...400, step: 5)
                            Text("\(Int(motionSensitivity))%")
                                .monospacedDigit()
                                .frame(minWidth: 52, alignment: .trailing)
                        }
                    }
                    .disabled(model.motionSource == .off || model.wideView)
                    if model.motionSource == .headTracking {
                        Button("Recenter Head Tracking", systemImage: "scope") {
                            model.recenterHeadTracking()
                        }
                        if model.headTrackingPaused {
                            Button("Resume Head Tracking", systemImage: "play") {
                                model.resumeHeadTracking()
                            }
                        }
                    }
                } header: {
                    Text("Motion controls")
                } footer: {
                    Text(motionFooter)
                }
                Section("Audio") {
                    HStack {
                        Image(systemName: "speaker.fill")
                        Slider(value: $volume, in: 0...100, step: 1)
                        Image(systemName: "speaker.wave.3.fill")
                    }
                    .accessibilityLabel("Volume")
                }
                Section {
                    Picker("Game language", selection: $language) {
                        Text("English").tag(1)
                        Text("Français").tag(2)
                        Text("Español").tag(5)
                    }
                    if let status = languageStatus {
                        Text(status).foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Game")
                }
                Section {
                    Toggle("Use JIT when available", isOn: $allowJIT)
                    LabeledContent("Running", value: cpuStatus)
                    if needsRestart {
                        Button("Restart Game Now", systemImage: "arrow.clockwise") {
                            model.restart()
                        }
                    }
                } header: {
                    Text("CPU")
                } footer: {
                    Text(emulator.aotLinked
                        ? "The JIT is fastest but is only available when Dekui is started "
                            + "through a JIT enabler such as StikDebug. Without it, Dekui runs "
                            + "the game's ahead-of-time translated code. Changes apply when the "
                            + "game restarts."
                        : "The JIT is only available when Dekui is started through a JIT "
                            + "enabler such as StikDebug; without it, Dekui uses the much "
                            + "slower interpreter. Changes apply when the game restarts.")
                }
                Section {
                    Button("Open Game Options", systemImage: "slider.horizontal.3") {
                        emulator.toggleDekuiSettings()
                    }
                    .disabled(model.state != .running && model.state != .paused)
                    Button("Controls…", systemImage: "gamecontroller") {
                        openWindow.panel("controls")
                    }
                } header: {
                    Text("Dekui")
                } footer: {
                    Text("Game Options shows Dekui's in-game settings for the HUD, menus, "
                        + "frame rate and gameplay over the game window.")
                }
                Section("About") {
                    LabeledContent("Dekui folder", value: "Files › On My Apple Vision Pro › Dekui")
                    Button("Licenses", systemImage: "doc.text") {
                        openWindow.panel("licenses")
                    }
                }
            }
            .navigationTitle("Settings")
        }
        .onAppear(perform: load)
        // visionOS can restore this window before the settings have loaded.
        .onChange(of: model.setup) { load() }
        .onChange(of: allowJIT) { if loaded { emulator.allowJIT = allowJIT } }
        .onChange(of: resolution) { if loaded { emulator.resolution = resolution } }
        .onChange(of: volume) { if loaded { emulator.volume = Int(volume) } }
        .onChange(of: vsync) { if loaded { emulator.vsync = vsync } }
        .onChange(of: diskShaderCache) { if loaded { emulator.diskShaderCache = diskShaderCache } }
        .onChange(of: hardwareShaders) { if loaded { emulator.hardwareShaders = hardwareShaders } }
        .onChange(of: asyncShaders) { if loaded { emulator.asyncShaders = asyncShaders } }
        .onChange(of: customTextures) { if loaded { emulator.customTextures = customTextures } }
        .onChange(of: language) { if loaded { emulator.gameLanguage = language } }
        .onChange(of: stereoscopic) {
            if loaded && emulator.stereoscopic != stereoscopic {
                emulator.stereoscopic = stereoscopic
                depth3D = Double(emulator.depth3D)
            }
        }
        .onChange(of: depth3D) { if loaded { emulator.depth3D = Int(depth3D) } }
        .onChange(of: motionSensitivity) { if loaded { DKMotion.sensitivity = Int(motionSensitivity) } }
        // The game window's ornament also toggles 3D.
        .onChange(of: model.stereoscopic) { stereoscopic = model.stereoscopic }
    }

    private var motionFooter: String {
        if model.wideView && model.motionSource != .off {
            return "Immersive view uses 50% sensitivity, so first-person aiming turns about "
                + "as far as your head."
        }
        switch model.motionSource {
        case .headTracking:
            return "Your head turns the 3DS for gyro aiming in first person (bow, hookshot "
                + "and other items). Head tracking keeps Dekui's immersive space open, which "
                + "hides other apps. Recenter takes the way you hold your head now as holding "
                + "the 3DS level."
        case .controller:
            return "Turning the controller turns the 3DS for gyro aiming in first person (bow, "
                + "hookshot and other items). Works with controllers that have a gyroscope, "
                + "such as DualSense, DualShock 4 and Switch Pro."
        default:
            return "The 3DS motion sensors report a console lying still."
        }
    }

    private var languageStatus: String? {
        switch emulator.gameLanguageStatus {
        case 1: return "Saving…"
        case 2: return "Saved. The game uses the new language after it restarts."
        case 3: return "The language could not be saved."
        default: return nil
        }
    }

    private func load() {
        guard model.setup == .ready else { return }
        loaded = false
        resolution = emulator.resolution
        volume = Double(emulator.volume)
        vsync = emulator.vsync
        diskShaderCache = emulator.diskShaderCache
        hardwareShaders = emulator.hardwareShaders
        allowJIT = emulator.allowJIT
        asyncShaders = emulator.asyncShaders
        customTextures = emulator.customTextures
        language = emulator.gameLanguage
        stereoscopic = emulator.stereoscopic
        depth3D = Double(emulator.depth3D)
        motionSensitivity = Double(DKMotion.sensitivity)
        DispatchQueue.main.async { loaded = true }
    }
}

/// Third-party licenses shipped in the Dekui folder.
struct LicensesView: View {
    @Environment(EmulatorModel.self) private var model

    var body: some View {
        NavigationStack {
            List(files, id: \.self) { url in
                NavigationLink(url.lastPathComponent) {
                    ScrollView {
                        Text((try? String(contentsOf: url, encoding: .utf8)) ?? "")
                            .font(.system(.body, design: .monospaced))
                            .padding()
                    }
                    .navigationTitle(url.lastPathComponent)
                }
            }
            .overlay {
                if files.isEmpty {
                    ContentUnavailableView("No licenses", systemImage: "doc.text",
                                           description: Text("The licenses folder is missing from the Dekui folder."))
                }
            }
            .navigationTitle("Licenses")
        }
    }

    private var files: [URL] {
        let folder = model.gameFolder.appendingPathComponent("licenses", isDirectory: true)
        let contents = (try? FileManager.default.contentsOfDirectory(
            at: folder, includingPropertiesForKeys: nil)) ?? []
        return contents.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }
}
