// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Dekui's menu over the paused game. Back closes it.
struct TVMenuView: View {
    @Environment(TVModel.self) private var model
    @State private var confirmRestart = false
    @State private var confirmQuit = false

    var body: some View {
        @Bindable var model = model
        NavigationStack(path: $model.menuPath) {
            HStack(alignment: .top, spacing: 80) {
                VStack(alignment: .leading, spacing: 12) {
                    Text("Dekui")
                        .font(.largeTitle.bold())
                    Text(statusText)
                        .foregroundStyle(.secondary)
                }
                .frame(width: 520, alignment: .leading)

                List {
                    Button {
                        model.menuShown = false
                    } label: {
                        Label("Resume", systemImage: "play.fill")
                    }
                    Button {
                        model.openGameOptions()
                    } label: {
                        Label("Game Options", systemImage: "slider.horizontal.3")
                    }
                    .disabled(!model.gameRunning || !model.emulator.dekuiSettingsAvailable)
                    NavigationLink(value: TVModel.Page.saveStates) {
                        Label("Save States", systemImage: "square.stack.3d.down.right")
                    }
                    NavigationLink(value: TVModel.Page.settings) {
                        Label("Settings", systemImage: "gearshape")
                    }
                    NavigationLink(value: TVModel.Page.controls) {
                        Label("Controls", systemImage: "gamecontroller")
                    }
                    Button {
                        confirmRestart = true
                    } label: {
                        Label("Restart Game", systemImage: "arrow.counterclockwise")
                    }
                    .disabled(!model.gameRunning)
                    Button(role: .destructive) {
                        confirmQuit = true
                    } label: {
                        Label("Quit Dekui", systemImage: "power")
                    }
                }
                .frame(width: 760)
            }
            .padding(80)
            .navigationDestination(for: TVModel.Page.self) { page in
                switch page {
                case .saveStates: TVSaveStatesView()
                case .settings: TVSettingsView()
                case .controls: TVControlsView()
                case .licenses: TVLicensesView()
                }
            }
        }
        .background(.ultraThinMaterial)
        .onExitCommand { model.menuShown = false }
        .confirmationDialog("Restart the game?", isPresented: $confirmRestart) {
            Button("Restart", role: .destructive) { model.restart() }
        } message: {
            Text("Save your game first. Progress since your last save will be lost.")
        }
        .confirmationDialog("Quit Dekui?", isPresented: $confirmQuit) {
            Button("Quit", role: .destructive) { model.quit() }
        } message: {
            Text("Save your game first. Progress since your last save will be lost.")
        }
    }

    private var statusText: String {
        switch model.state {
        case .running, .paused: return "The game is paused while this menu is open."
        case .loading: return "The game is loading."
        default: return "The game is not running."
        }
    }
}

// MARK: Settings

/// Emulator settings, saved to user/config/qt-config.ini like the other apps.
struct TVSettingsView: View {
    @Environment(TVModel.self) private var model

    @State private var resolution = 2
    @State private var volume = 100
    @State private var vsync = true
    @State private var diskShaderCache = true
    @State private var hardwareShaders = false
    @State private var asyncShaders = false
    @State private var customTextures = false
    @State private var language = 1
    @State private var motionSource = DKMotionSource.controller
    @State private var motionSensitivity = 100
    @State private var loaded = false

    private var emulator: DKEmulator { model.emulator }

    var body: some View {
        Form {
            Section("Graphics") {
                Picker("Internal resolution", selection: $resolution) {
                    ForEach(1...6, id: \.self) { scale in
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
                Picker("Motion controls", selection: $motionSource) {
                    Text("Off").tag(DKMotionSource.off)
                    Text("Controller gyro").tag(DKMotionSource.controller)
                }
                Picker("Sensitivity", selection: $motionSensitivity) {
                    ForEach([25, 50, 75, 100, 150, 200, 300, 400], id: \.self) { value in
                        Text("\(value)%").tag(value)
                    }
                }
                .disabled(motionSource == .off)
            } header: {
                Text("Motion controls")
            } footer: {
                Text("Turning the controller turns the 3DS for gyro aiming in first person. "
                    + "Works with controllers that have a gyroscope, such as DualSense and "
                    + "DualShock 4.")
            }
            Section("Audio") {
                Picker("Volume", selection: $volume) {
                    ForEach(volumeChoices, id: \.self) { value in
                        Text("\(value)%").tag(value)
                    }
                }
            }
            Section("Game") {
                Picker("Game language", selection: $language) {
                    Text("English").tag(1)
                    Text("Français").tag(2)
                    Text("Español").tag(5)
                }
                if let status = languageStatus {
                    Text(status).foregroundStyle(.secondary)
                }
            }
            Section("About") {
                LabeledContent("CPU", value: "Interpreter with ahead-of-time code")
                LabeledContent("Dekui folder", value: model.gameFolder.path)
                NavigationLink("Licenses", value: TVModel.Page.licenses)
            }
        }
        .navigationTitle("Settings")
        .onAppear(perform: load)
        .onChange(of: resolution) { if loaded && emulator.resolution != resolution { emulator.resolution = resolution } }
        .onChange(of: volume) { if loaded && emulator.volume != volume { emulator.volume = volume } }
        .onChange(of: vsync) { if loaded && emulator.vsync != vsync { emulator.vsync = vsync } }
        .onChange(of: diskShaderCache) { if loaded && emulator.diskShaderCache != diskShaderCache { emulator.diskShaderCache = diskShaderCache } }
        .onChange(of: hardwareShaders) { if loaded && emulator.hardwareShaders != hardwareShaders { emulator.hardwareShaders = hardwareShaders } }
        .onChange(of: asyncShaders) { if loaded && emulator.asyncShaders != asyncShaders { emulator.asyncShaders = asyncShaders } }
        .onChange(of: customTextures) { if loaded && emulator.customTextures != customTextures { emulator.customTextures = customTextures } }
        .onChange(of: language) { if loaded && emulator.gameLanguage != language { emulator.gameLanguage = language } }
        .onChange(of: motionSource) { if loaded && DKMotion.source != motionSource { DKMotion.source = motionSource } }
        .onChange(of: motionSensitivity) { if loaded && DKMotion.sensitivity != motionSensitivity { DKMotion.sensitivity = motionSensitivity } }
    }

    /// Steps of 10, plus the current volume if it is between them.
    private var volumeChoices: [Int] {
        Array(Set(stride(from: 0, through: 100, by: 10)).union([volume])).sorted()
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
        volume = emulator.volume
        vsync = emulator.vsync
        diskShaderCache = emulator.diskShaderCache
        hardwareShaders = emulator.hardwareShaders
        asyncShaders = emulator.asyncShaders
        customTextures = emulator.customTextures
        language = emulator.gameLanguage
        motionSource = DKMotion.source == .headTracking ? .controller : DKMotion.source
        motionSensitivity = DKMotion.sensitivity
        DispatchQueue.main.async { loaded = true }
    }
}

/// Third-party licenses shipped in the Dekui folder.
struct TVLicensesView: View {
    @Environment(TVModel.self) private var model

    var body: some View {
        List(files, id: \.self) { url in
            NavigationLink(url.lastPathComponent) {
                ScrollView {
                    Text((try? String(contentsOf: url, encoding: .utf8)) ?? "")
                        .font(.system(.caption, design: .monospaced))
                        .padding()
                        .focusable()
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

    private var files: [URL] {
        let folder = model.gameFolder.appendingPathComponent("licenses", isDirectory: true)
        let contents = (try? FileManager.default.contentsOfDirectory(
            at: folder, includingPropertiesForKeys: nil)) ?? []
        return contents.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }
}
