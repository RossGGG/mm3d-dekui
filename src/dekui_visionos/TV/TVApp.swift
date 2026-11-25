// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Dekui for Apple TV: the game fills the screen; the Siri Remote's Back or
/// Play/Pause button, or both stick clicks (L3+R3), open Dekui's menu.
@main
struct DekuiTVApp: App {
    @State private var model = TVModel()

    init() {
        KeyboardPrompt.shared.install()
    }

    var body: some Scene {
        WindowGroup {
            TVRootView()
                .environment(model)
        }
    }
}

struct TVRootView: View {
    @Environment(TVModel.self) private var model
    @State private var keyboard = KeyboardPrompt.shared
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        @Bindable var model = model
        ZStack {
            Color.black.ignoresSafeArea()
            switch model.setup {
            case .ready:
                TVGameScreen()
            case .checking:
                ProgressView("Checking the game…")
            case .missingGame(let text), .failed(let text):
                TVSetupView(message: text)
            }
        }
        .fullScreenCover(isPresented: $model.menuShown) {
            TVMenuView()
                .environment(model)
        }
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
        .alert(item: $model.message) { message in
            Alert(title: Text(message.title), message: Text(message.text))
        }
        .task { model.checkSetup() }
        .onChange(of: scenePhase) { _, phase in
            model.scenePhaseChanged(phase)
        }
    }
}

/// Shown until the game and a prepared user folder are on the Apple TV.
struct TVSetupView: View {
    @Environment(TVModel.self) private var model
    let message: String

    var body: some View {
        VStack(spacing: 40) {
            Image(systemName: "gamecontroller")
                .font(.system(size: 96))
                .foregroundStyle(.secondary)
            Text("Dekui needs your game")
                .font(.title)
            Text(message)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 1200)
                .foregroundStyle(.secondary)
            Text(model.gameFolder.path)
                .font(.caption.monospaced())
                .foregroundStyle(.tertiary)
            Button("Check Again", systemImage: "arrow.clockwise") {
                model.checkSetup()
            }
        }
        .padding(80)
    }
}
