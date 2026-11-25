// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Observation
import SwiftUI

/// Text the game asks for through the 3DS software keyboard, such as the name
/// of a new save file. The game waits until the prompt is answered.
@MainActor
@Observable
final class KeyboardPrompt {
    static let shared = KeyboardPrompt()

    private(set) var request: DKKeyboardRequest?
    var gameError: String?

    /// Routes the emulator's keyboard requests here. Call once at launch.
    func install() {
        DKSoftwareKeyboard.presenter = { request in
            MainActor.assumeIsolated { KeyboardPrompt.shared.request = request }
        }
        DKSoftwareKeyboard.errorPresenter = { message in
            MainActor.assumeIsolated { KeyboardPrompt.shared.gameError = message }
        }
    }

    fileprivate func finish(text: String, button: Int) {
        request?.finish(withText: text, button: button)
        request = nil
    }
}

/// The sheet shown over the game window while the game waits for text.
struct KeyboardPromptView: View {
    let request: DKKeyboardRequest

    @State private var text = ""
    @State private var error: String?
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text(request.hint.isEmpty ? "Enter a name" : request.hint)
                .font(.title2)
            TextField("Name", text: $text, axis: request.multiline ? .vertical : .horizontal)
#if !os(tvOS)
                .textFieldStyle(.roundedBorder)
#endif
                .autocorrectionDisabled()
                .focused($focused)
                .onSubmit { press(request.buttons.count - 1) }
                .onChange(of: text) {
                    let limit = request.maxLength
                    if limit > 0, text.count > limit { text = String(text.prefix(limit)) }
                    error = nil
                }
            if request.maxLength > 0 {
                Text("\(text.count) of \(request.maxLength) characters")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if let error {
                Label(error, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
            }
            HStack {
                Spacer()
                ForEach(Array(request.buttons.enumerated()), id: \.offset) { index, title in
                    let confirms = index == request.buttons.count - 1
                    Button(title) { press(index) }
                        .buttonStyle(.borderedProminent)
                        .tint(confirms ? .accentColor : .secondary)
                }
            }
        }
        .padding(32)
#if os(tvOS)
        .frame(width: 1000)
#else
        .frame(width: 520)
#endif
        .onAppear { focused = true }
        // Leaving without a choice would leave the game waiting.
        .interactiveDismissDisabled()
    }

    private func press(_ index: Int) {
        if let message = request.validationError(forText: text, button: index) {
            error = message
            return
        }
        KeyboardPrompt.shared.finish(text: text, button: index)
    }
}
