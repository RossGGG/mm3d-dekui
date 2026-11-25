// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import GameController
import SwiftUI
import UIKit

/// The emulator's render target. Touches (direct, or gaze and pinch) drive the
/// Dekui settings overlay; hardware keyboard presses go to the game.
final class GameHostView: UIView {
    /// A sublayer rather than the view's backing layer: MoltenVK configures the
    /// layer from the emulation thread, which UIKit forbids for backing layers.
    let metalLayer = CAMetalLayer()

    var onLayout: ((CAMetalLayer, CGSize) -> Void)?
    var onWindowScene: ((UIWindowScene) -> Void)?
    /// Called when a pinch begins; returning true keeps that touch from the game.
    var onPinch: (() -> Bool)?
    /// False during the immersive view, where the game is on the curved screen:
    /// the window then turns see-through instead of showing an empty black
    /// render target. Done here rather than with SwiftUI opacity, which (with
    /// the plain window) left the curved screen blank on device.
    var showsGame = true {
        didSet {
            guard showsGame != oldValue else { return }
            backgroundColor = showsGame ? .black : .clear
            metalLayer.isHidden = !showsGame
        }
    }
    private var swallowingTouch = false
    private let emulator = DKEmulator.shared

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = false
        layer.addSublayer(metalLayer)
        layer.masksToBounds = true
        // Present one pixel per point: the 3DS image is already upscaled, and
        // the headset's backing scale would only cost fill rate.
        metalLayer.contentsScale = 1
        metalLayer.framebufferOnly = true
        backgroundColor = .black

        // Without this, visionOS uses controller buttons to activate the UI
        // element the player looks at. While the game view has focus, deliver
        // them to the game through the GameController framework instead.
        let controllerInput = GCEventInteraction()
        controllerInput.handledEventTypes = .gamepad
        addInteraction(controllerInput)

        let scroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        scroll.allowedScrollTypesMask = .all
        scroll.allowedTouchTypes = []
        addGestureRecognizer(scroll)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override func layoutSubviews() {
        super.layoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        metalLayer.frame = bounds
        metalLayer.contentsScale = 1
        CATransaction.commit()
        guard bounds.width > 0, bounds.height > 0 else { return }
        emulator.setViewSize(bounds.size)
        onLayout?(metalLayer, bounds.size)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard let window else { return }
        becomeFirstResponder()
        if let scene = window.windowScene { onWindowScene?(scene) }
        // Resize the game window only in proportion, keeping its 16:9 shape.
        window.windowScene?.requestGeometryUpdate(
            .Vision(resizingRestrictions: .uniform)) { error in
                print("Could not lock the game window's aspect ratio: \(error)")
            }
    }

    // MARK: Pointer

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        becomeFirstResponder()
        guard let touch = touches.first else { return }
        swallowingTouch = onPinch?() ?? false
        guard !swallowingTouch else { return }
        emulator.pointerDown(at: touch.location(in: self))
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard !swallowingTouch, let touch = touches.first else { return }
        emulator.pointerMoved(to: touch.location(in: self))
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard !swallowingTouch else {
            swallowingTouch = false
            return
        }
        guard let touch = touches.first else { return }
        emulator.pointerUp(at: touch.location(in: self))
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        touchesEnded(touches, with: event)
    }

    @objc private func scrolled(_ recognizer: UIPanGestureRecognizer) {
        let translation = recognizer.translation(in: self)
        recognizer.setTranslation(.zero, in: self)
        emulator.scroll(at: recognizer.location(in: self),
                        delta: CGVector(dx: translation.x / 40, dy: translation.y / 40))
    }

    // MARK: Keyboard

    override var canBecomeFirstResponder: Bool { true }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var unhandled = Set<UIPress>()
        for press in presses {
            guard let key = press.key, emulator.handle(key, down: true) else {
                unhandled.insert(press)
                continue
            }
        }
        if !unhandled.isEmpty { super.pressesBegan(unhandled, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var unhandled = Set<UIPress>()
        for press in presses {
            guard let key = press.key, emulator.handle(key, down: false) else {
                unhandled.insert(press)
                continue
            }
        }
        if !unhandled.isEmpty { super.pressesEnded(unhandled, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        pressesEnded(presses, with: event)
    }

    override func resignFirstResponder() -> Bool {
        emulator.releaseAllKeys()
        return super.resignFirstResponder()
    }
}

struct GameView: UIViewRepresentable {
    let model: EmulatorModel

    func makeUIView(context: Context) -> GameHostView {
        let view = GameHostView(frame: .zero)
        view.onLayout = { layer, size in
            MainActor.assumeIsolated { model.start(layer: layer, size: size) }
        }
        view.onWindowScene = { scene in
            MainActor.assumeIsolated { model.gameScene = scene }
        }
        view.onPinch = {
            MainActor.assumeIsolated { model.gamePinched() }
        }
        return view
    }

    func updateUIView(_ view: GameHostView, context: Context) {
        view.showsGame = !model.wideView
    }
}

/// The game window: the render target, a loading indicator until the first
/// frame, and the ornament with Dekui's controls.
struct GameScreen: View {
    @Environment(EmulatorModel.self) private var model
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        ZStack {
            GameView(model: model)
                .ignoresSafeArea()
                // The window is plain (see DekuiApp), so round the game's corners
                // as a glass window would.
                .clipShape(.rect(cornerRadius: 46))
            // The stereo plane draws over SwiftUI content, so it waits for the
            // game: until then the loading indicator must show.
            if model.stereoscopic && !model.wideView && model.state != .loading {
                StereoGameView(screen: model.stereoScreen)
            }
            if model.wideView {
                ImmersiveControlsHandle()
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
            }
            if model.state == .loading {
                VStack(spacing: 16) {
                    ProgressView(value: model.loadFraction)
                        .frame(width: 320)
                    Text(model.loadMessage)
                        .font(.headline)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 420)
                }
                .padding(32)
                .glassBackgroundEffect()
                .offset(z: 4) // in front of the stereo screen
            } else if model.state == .paused {
                Label("Paused", systemImage: "pause.fill")
                    .font(.title)
                    .padding(24)
                    .glassBackgroundEffect()
                    .offset(z: 4)
            }
        }
        .aspectRatio(16.0 / 9.0, contentMode: .fit)
        .onChange(of: model.state) { old, state in
            // `-DekuiShowGameOptions YES` opens the in-game options once the
            // game is running, for screenshots and UI tests.
            if old == .loading, state == .running,
               UserDefaults.standard.bool(forKey: "DekuiShowGameOptions") {
                model.emulator.toggleDekuiSettings()
            }
            // `-DekuiOpenSettings YES` opens Settings once the game runs.
            if old == .loading, state == .running,
               UserDefaults.standard.bool(forKey: "DekuiOpenSettings") {
                openWindow.panel("settings")
            }
            // `-DekuiImmersiveView YES` turns on the immersive view once the game runs.
            if old == .loading, state == .running,
               UserDefaults.standard.bool(forKey: "DekuiImmersiveView") {
                model.wideView = true
            }
        }
        .persistentSystemOverlays((model.fullScreen || model.wideView) && !model.ornamentVisible
                                  ? .hidden : .automatic)
        .ornament(visibility: model.ornamentVisible ? .visible : .hidden,
                  attachmentAnchor: .scene(.bottom), contentAlignment: .top) {
            GameOrnament()
        }
    }
}

/// The game window during the immersive view, where the game is on the curved
/// screen instead: a short hint while the controls are up, then only a faint
/// handle that brings them back. The window is see-through meanwhile (see
/// GameHostView.showsGame); fading GameView with SwiftUI opacity instead left
/// the curved screen blank.
struct ImmersiveControlsHandle: View {
    @Environment(EmulatorModel.self) private var model
    @State private var lookAhead = false

    var body: some View {
        Group {
            if model.ornamentVisible {
                Text(lookAhead ? "Look straight ahead for a moment."
                     : model.hudAdjusting
                     ? "Pinch and drag the HUD to move it. Pinch with both hands to resize it."
                     : "The controls fade away. Look here and pinch to bring them back.")
                    .font(.callout)
                    .padding(.horizontal, 20)
                    .padding(.vertical, 10)
                    .glassBackgroundEffect(in: .capsule)
            } else {
                Button {
                    model.showOrnament()
                } label: {
                    Label("Show Controls", systemImage: "ellipsis")
                }
                .labelStyle(.iconOnly)
                .buttonBorderShape(.circle)
                // Faint until looked at.
                .hoverEffect { effect, isActive, _ in effect.opacity(isActive ? 1 : 0.3) }
            }
        }
        .transition(.opacity)
        .padding(.bottom, 16)
        // Entering the view and Recenter both place the screen where the head
        // points a moment later.
        .task(id: model.recenterCount) {
            lookAhead = true
            try? await Task.sleep(for: .seconds(2))
            withAnimation { lookAhead = false }
        }
    }
}

struct GameOrnament: View {
    @Environment(EmulatorModel.self) private var model
    @Environment(\.openWindow) private var openWindow
    @State private var confirmQuit = false
    @State private var confirmRestart = false

    var body: some View {
        HStack(spacing: 12) {
            Button {
                model.togglePause()
            } label: {
                Label(model.state == .paused ? "Resume" : "Pause",
                      systemImage: model.state == .paused ? "play.fill" : "pause.fill")
            }
            .disabled(model.state != .running && model.state != .paused)

            Button {
                model.emulator.toggleDekuiSettings()
            } label: {
                Label("Game Options", systemImage: "slider.horizontal.3")
            }
            .disabled(model.state != .running && model.state != .paused)

            Toggle(isOn: Binding(get: { model.stereoscopic },
                                 set: { _ in model.toggleStereoscopic() })) {
                Label("3D", systemImage: "view.3d")
            }
            .toggleStyle(.button)

            Button {
                confirmRestart = true
            } label: {
                Label("Restart Game", systemImage: "arrow.counterclockwise")
            }
            .disabled(model.state != .running && model.state != .paused)
            .confirmationDialog("Restart the game?", isPresented: $confirmRestart) {
                Button("Restart", role: .destructive) { model.restart() }
            } message: {
                Text("Save your game first. Progress since your last save will be lost.")
            }

            // The immersive view has no game window to enlarge and already
            // tints the room.
            if !model.wideView {
                Toggle(isOn: Binding(get: { model.fullScreen },
                                     set: { _ in model.toggleFullScreen() })) {
                    Label("Full Screen", systemImage: model.fullScreen
                          ? "arrow.down.right.and.arrow.up.left"
                          : "arrow.up.left.and.arrow.down.right")
                }
                .toggleStyle(.button)

                // RootView opens or closes the immersive space that dims the room.
                Toggle(isOn: Binding(get: { model.surroundingsDimmed },
                                     set: { model.dimRequested = $0 })) {
                    Label("Dim Surroundings", systemImage: "moon")
                }
                .toggleStyle(.button)
            }

            Toggle(isOn: Binding(get: { model.wideView },
                                 set: { model.wideView = $0 })) {
                Label("Immersive View", systemImage: "pano")
            }
            .toggleStyle(.button)
            .disabled(model.state != .running && model.state != .paused)

            if model.motionSource == .headTracking || model.wideView {
                Button {
                    model.recenterHeadTracking()
                } label: {
                    Label("Recenter", systemImage: "scope")
                }
            }

            // While on, pinching the HUD moves and scales it instead of
            // reaching the game.
            if model.wideView {
                Toggle(isOn: Binding(get: { model.hudAdjusting },
                                     set: { model.hudAdjusting = $0 })) {
                    Label("Adjust HUD", systemImage: "rectangle.and.hand.point.up.left")
                }
                .toggleStyle(.button)

                if model.hudAdjusting {
                    Button {
                        model.resetHud()
                    } label: {
                        Label("Reset HUD", systemImage: "arrow.uturn.backward.circle")
                    }
                    .disabled(!model.hudAdjusted)

                    Button("Done") { model.hudAdjusting = false }
                        .labelStyle(.titleOnly)
                }
            }

            Button {
                openWindow.panel("savestates")
            } label: {
                Label("Save States", systemImage: "square.stack.3d.down.right")
            }

            Button {
                openWindow.panel("controls")
            } label: {
                Label("Controls", systemImage: "gamecontroller")
            }

            Button {
                openWindow.panel("settings")
            } label: {
                Label("Settings", systemImage: "gearshape")
            }

            Button(role: .destructive) {
                confirmQuit = true
            } label: {
                Label("Quit Dekui", systemImage: "power")
            }
            .confirmationDialog("Quit Dekui?", isPresented: $confirmQuit) {
                Button("Quit", role: .destructive) { model.quit() }
            } message: {
                Text("Save your game first. Progress since your last save will be lost.")
            }
        }
        .labelStyle(.iconOnly)
        .padding(12)
        .glassBackgroundEffect()
        // Using the ornament restarts its hide timer; its dialogs keep it up.
        .simultaneousGesture(TapGesture().onEnded { model.showOrnament() })
        .onChange(of: confirmQuit) { _, shown in model.holdOrnament(shown) }
        .onChange(of: confirmRestart) { _, shown in model.holdOrnament(shown) }
        // Ornaments overlap the window's edge, and the stereo screen draws
        // over that part even on the window's surface: keep it fully below.
        .padding(.top, model.stereoscopic ? 28 : 0)
    }
}
