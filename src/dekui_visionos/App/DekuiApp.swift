// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

@main
struct DekuiApp: App {
    @State private var model: EmulatorModel
    @State private var dimStyle: any ImmersionStyle = .mixed

    init() {
        // Ask for JIT memory before anything else, while a JIT enabler's
        // debugger may still be attached.
        DKEmulator.initializeJit()
        _model = State(initialValue: EmulatorModel())
        KeyboardPrompt.shared.install()
    }

    var body: some Scene {
        WindowGroup(id: "game") {
            RootView()
                .environment(model)
        }
        .defaultSize(width: 1280, height: 720)
        // Plain, with RootView drawing the glass, so the immersive view can
        // make the window see-through.
        .windowStyle(.plain)
        // Content sets only the minimum, so the window keeps the size the
        // player chose instead of snapping to the setup check's content.
        .windowResizability(.contentMinSize)

        // One window per id: OpenWindowAction.panel passes a fixed value, so reopening
        // brings the existing window forward.
        WindowGroup("Settings", id: "settings", for: String.self) { _ in
            SettingsView()
                .environment(model)
        }
        .defaultSize(width: 640, height: 720)

        WindowGroup("Controls", id: "controls", for: String.self) { _ in
            ControlsView()
                .environment(model)
        }
        .defaultSize(width: 640, height: 760)

        WindowGroup("Save States", id: "savestates", for: String.self) { _ in
            SaveStatesView()
                .environment(model)
        }
        .defaultSize(width: 640, height: 760)

        WindowGroup("Licenses", id: "licenses", for: String.self) { _ in
            LicensesView()
                .environment(model)
        }
        .defaultSize(width: 720, height: 720)

        ImmersiveSpace(id: DimSurroundings.id) {
            DimSurroundings()
                .environment(model)
        }
        .immersionStyle(selection: $dimStyle, in: .mixed)
    }
}

extension OpenWindowAction {
    /// Opens a single-instance auxiliary window (settings, controls, licenses).
    func panel(_ id: String) {
        callAsFunction(id: id, value: id)
    }
}

struct RootView: View {
    @Environment(EmulatorModel.self) private var model
    @State private var keyboard = KeyboardPrompt.shared
    @Environment(\.openWindow) private var openWindow
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @State private var changingImmersiveSpace = false

    var body: some View {
        @Bindable var model = model
        Group {
            switch model.setup {
            case .ready:
                GameScreen()
            case .checking:
                ProgressView("Checking the game…")
                    .padding(40)
            case .missingGame(let text), .failed(let text):
                SetupView(message: text)
            }
        }
        .frame(minWidth: 640, maxWidth: .infinity, minHeight: 360, maxHeight: .infinity)
        .glassBackgroundEffect(in: .rect(cornerRadius: 46),
                               displayMode: model.wideView ? .never : .always)
        .sheet(isPresented: Binding(get: { keyboard.request != nil }, set: { _ in })) {
            if let request = keyboard.request {
                KeyboardPromptView(request: request)
            }
        }
        .alert("The game did not accept that text",
               isPresented: Binding(get: { keyboard.gameError != nil },
                                    set: { if !$0 { keyboard.gameError = nil } })) {
            Button("OK") {}
        } message: {
            Text(keyboard.gameError ?? "")
        }
        .task { model.checkSetup() }
        .onChange(of: scenePhase) { _, phase in
            model.scenePhaseChanged(phase)
        }
        .onChange(of: model.setup) { _, setup in
            // Launch arguments for screenshots and UI tests, e.g.
            // `-DekuiOpenPanel settings` (settings, controls or licenses).
            if setup == .ready, let panel = UserDefaults.standard.string(forKey: "DekuiOpenPanel") {
                openWindow.panel(panel)
            }
        }
        // Dimming and head-tracked motion controls share one immersive space.
        .onChange(of: model.immersiveSpaceRequests) { Task { await syncImmersiveSpace() } }
        .onChange(of: model.setup) { _, setup in
            if setup == .ready { Task { await syncImmersiveSpace() } }
        }
        .onChange(of: model.controlsRequests) { openWindow.panel("controls") }
        .onChange(of: model.licensesRequests) { openWindow.panel("licenses") }
        .alert(item: $model.message) { message in
            Alert(title: Text(message.title), message: Text(message.text))
        }
    }

    /// Opens or closes Dekui's immersive space to match what the player chose.
    private func syncImmersiveSpace() async {
        guard model.setup == .ready, !changingImmersiveSpace else { return }
        changingImmersiveSpace = true
        defer { changingImmersiveSpace = false }
        if model.immersiveSpaceWanted && !model.immersiveSpaceOpen {
            switch await openImmersiveSpace(id: DimSurroundings.id) {
            case .opened: break
            default: model.immersiveSpaceFailed()
            }
        } else if !model.immersiveSpaceWanted && model.immersiveSpaceOpen {
            await dismissImmersiveSpace()
        }
    }
}
