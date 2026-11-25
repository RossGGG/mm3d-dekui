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

/// Controller mapping for the current input profile.
struct TVControlsView: View {
    @Environment(TVModel.self) private var model

    @State private var bindings: [BindingTarget: String] = [:]
    @State private var capturing: BindingTarget?
    @State private var confirmReset = false

    private static let groups: [(String, [(DKControl, String)])] = [
        ("Buttons", [
            (.buttonA, "A"), (.buttonB, "B"), (.buttonX, "X"), (.buttonY, "Y"),
            (.buttonL, "L"), (.buttonR, "R"), (.buttonZL, "ZL"), (.buttonZR, "ZR"),
            (.buttonStart, "Start"), (.buttonSelect, "Select"),
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
                Text("A connected controller is mapped automatically. Changing a binding keeps "
                    + "your mapping until you return to automatic. Click both sticks (L3+R3) to "
                    + "open Dekui's menu.")
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
        .onAppear(perform: reload)
        .onChange(of: model.controllerNames) { reload() }
        .onDisappear { emulator.cancelCapture() }
        // While waiting for input, controller presses must reach only the
        // game's input layer, not move the focus or press the row again.
        .fullScreenCover(isPresented: Binding(get: { capturing != nil },
                                              set: { if !$0 { cancelCapture() } })) {
            TVCaptureView(prompt: capturePrompt)
                .onAppear { model.captureCancel = cancelCapture }
                .onDisappear { model.captureCancel = nil }
        }
        .confirmationDialog("Replace your bindings with the automatic mapping?",
                            isPresented: $confirmReset) {
            Button("Use Automatic Mapping", role: .destructive) {
                emulator.resetControlsToAutomatic()
                // Automatic mapping is applied on the next frames.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { reload() }
            }
        }
    }

    private var capturePrompt: String {
        guard let capturing else { return "" }
        return capturing.control.rawValue >= DKControl.circlePad.rawValue && capturing.direction == nil
            ? "Move the stick in a circle" : "Press a button"
    }

    private func row(_ target: BindingTarget, name: String) -> some View {
        Button {
            capture(target)
        } label: {
            LabeledContent(name) {
                let binding = bindings[target] ?? ""
                Text(binding.isEmpty ? "Not set" : binding)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func capture(_ target: BindingTarget) {
        capturing = target
        let done: (String?) -> Void = { binding in
            if binding != nil { reload() }
            if capturing == target { capturing = nil }
        }
        // Let the press that chose the row end first, so it is not captured.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            guard capturing == target else { return }
            if let direction = target.direction {
                emulator.captureBinding(forStick: target.control, direction: direction, completion: done)
            } else {
                emulator.captureBinding(for: target.control, completion: done)
            }
        }
    }

    private func cancelCapture() {
        emulator.cancelCapture()
        capturing = nil
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

/// Covers the screen while a binding is captured. Its view controller keeps
/// controller presses away from UIKit; the Siri Remote's Back reaches only the
/// Game Controller framework then, and TVControllerInput cancels.
private struct TVCaptureView: UIViewControllerRepresentable {
    let prompt: String

    final class Controller: GCEventViewController {
        let label = UILabel()

        override func viewDidLoad() {
            super.viewDidLoad()
            controllerUserInteractionEnabled = false
            view.backgroundColor = UIColor.black.withAlphaComponent(0.85)
            label.font = .preferredFont(forTextStyle: .title1)
            label.textColor = .white
            label.textAlignment = .center
            label.numberOfLines = 0
            label.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(label)
            NSLayoutConstraint.activate([
                label.centerXAnchor.constraint(equalTo: view.centerXAnchor),
                label.centerYAnchor.constraint(equalTo: view.centerYAnchor),
                label.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, multiplier: 0.8),
            ])
        }
    }

    func makeUIViewController(context: Context) -> Controller {
        Controller()
    }

    func updateUIViewController(_ controller: Controller, context: Context) {
        controller.label.text = "\(prompt) on your controller.\n\nPress Back on the Siri Remote to cancel."
    }
}
