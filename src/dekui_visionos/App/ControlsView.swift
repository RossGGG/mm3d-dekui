// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import GameController
import SwiftUI
import UIKit

/// What a capture binds: a button, a whole stick, or one direction of a stick.
private struct BindingTarget: Hashable {
    let control: DKControl
    var direction: DKStickDirection? = nil
}

/// Controller and keyboard mapping for the current input profile.
struct ControlsView: View {
    @Environment(EmulatorModel.self) private var model

    @State private var bindings: [BindingTarget: String] = [:]
    @State private var capturing: BindingTarget?
    @State private var confirmReset = false

    private static let groups: [(String, [(DKControl, String)])] = [
        ("Buttons", [
            (.buttonA, "A"), (.buttonB, "B"), (.buttonX, "X"), (.buttonY, "Y"),
            (.buttonL, "L"), (.buttonR, "R"), (.buttonZL, "ZL"), (.buttonZR, "ZR"),
            (.buttonStart, "Start"), (.buttonSelect, "Select"), (.buttonHome, "Home"),
        ]),
        ("D-Pad", [(.dPadUp, "Up"), (.dPadDown, "Down"), (.dPadLeft, "Left"), (.dPadRight, "Right")]),
        ("Sticks", [(.circlePad, "Circle Pad"), (.cStick, "C-Stick")]),
    ]
    private static let sticks: [(DKControl, String)] = [(.circlePad, "Circle Pad"), (.cStick, "C-Stick")]
    private static let directions: [(DKStickDirection, String)] = [
        (.up, "Up"), (.down, "Down"), (.left, "Left"), (.right, "Right"),
    ]

    private var emulator: DKEmulator { model.emulator }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    if model.controllerNames.isEmpty {
                        Label("No controller connected", systemImage: "gamecontroller")
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(Array(model.controllerNames.enumerated()), id: \.offset) { _, name in
                            Label(name, systemImage: "gamecontroller.fill")
                        }
                    }
                } header: {
                    Text("Controllers")
                } footer: {
                    Text("A connected controller is mapped automatically. Changing a binding "
                        + "keeps your mapping until you return to automatic.")
                }
                ForEach(Self.groups, id: \.0) { group in
                    Section(group.0) {
                        ForEach(group.1, id: \.0) { control, name in
                            row(BindingTarget(control: control), name: name)
                        }
                    }
                }
                ForEach(Self.sticks, id: \.0) { stick, stickName in
                    Section {
                        ForEach(Self.directions, id: \.0) { direction, name in
                            row(BindingTarget(control: stick, direction: direction), name: name)
                        }
                    } header: {
                        Text("\(stickName) Directions")
                    } footer: {
                        Text("Bind each direction to a button, D-pad direction or stick nudge. "
                            + "Binding a direction replaces a whole-stick binding.")
                    }
                }
                Section {
                    Button("Use Automatic Mapping", systemImage: "arrow.counterclockwise") {
                        confirmReset = true
                    }
                }
            }
            .navigationTitle("Controls")
        }
        .onAppear(perform: reload)
        // visionOS can restore this window before the settings have loaded.
        .onChange(of: model.setup) { reload() }
        .onChange(of: model.controllerNames) { reload() }
        .onDisappear { emulator.cancelCapture() }
        // While waiting for input, controller buttons must reach the game's
        // input layer instead of activating the row the player looks at.
        .background(ControllerInputRouting(active: capturing != nil))
        .confirmationDialog("Replace your bindings with the automatic mapping?",
                            isPresented: $confirmReset) {
            Button("Use Automatic Mapping", role: .destructive) {
                emulator.resetControlsToAutomatic()
                // Automatic mapping is applied on the next frames.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { reload() }
            }
        }
    }

    private func row(_ target: BindingTarget, name: String) -> some View {
        Button {
            capture(target)
        } label: {
            LabeledContent(name) {
                if capturing == target {
                    Text(target.control.rawValue >= DKControl.circlePad.rawValue && target.direction == nil
                         ? "Move the stick in a circle…" : "Press a button…")
                        .foregroundStyle(.tint)
                } else {
                    let binding = bindings[target] ?? ""
                    Text(binding.isEmpty ? "Not set" : binding)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    private func capture(_ target: BindingTarget) {
        capturing = target
        let done: (String?) -> Void = { binding in
            if binding != nil { reload() }
            if capturing == target { capturing = nil }
        }
        if let direction = target.direction {
            emulator.captureBinding(forStick: target.control, direction: direction, completion: done)
        } else {
            emulator.captureBinding(for: target.control, completion: done)
        }
    }

    private func reload() {
        guard model.setup == .ready else { return }
        var result: [BindingTarget: String] = [:]
        for group in Self.groups {
            for (control, _) in group.1 {
                result[BindingTarget(control: control)] = emulator.binding(for: control)
            }
        }
        for (stick, _) in Self.sticks {
            for (direction, _) in Self.directions {
                result[BindingTarget(control: stick, direction: direction)] =
                    emulator.binding(forStick: stick, direction: direction)
            }
        }
        bindings = result
    }
}

/// Sends game controller events to the Game Controller framework (and so to
/// the emulator) for the whole window while `active`.
private struct ControllerInputRouting: UIViewRepresentable {
    let active: Bool

    final class RoutingView: UIView {
        let interaction = GCEventInteraction()
        var active = false { didSet { update() } }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            update()
        }

        private func update() {
            interaction.view?.removeInteraction(interaction)
            if active, let window {
                interaction.handledEventTypes = .gamepad
                window.addInteraction(interaction)
            }
        }
    }

    func makeUIView(context: Context) -> RoutingView {
        let view = RoutingView()
        view.isUserInteractionEnabled = false
        return view
    }

    func updateUIView(_ view: RoutingView, context: Context) {
        view.active = active
    }
}
