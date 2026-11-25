// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GameController
import Observation
import QuartzCore
import SwiftUI

/// App-wide state for the Apple TV app.
@MainActor
@Observable
final class TVModel: NSObject, DKEmulatorHost {
    enum Setup: Equatable {
        case checking
        case ready
        case missingGame(String)
        case failed(String)
    }

    /// Pages of Dekui's menu.
    enum Page: Hashable {
        case saveStates, settings, controls, licenses
    }

    struct Message: Identifiable {
        let id = UUID()
        let title: String
        let text: String
    }

    let emulator = DKEmulator.shared
    private(set) var setup: Setup = .checking
    private(set) var state: DKEmulationState = .stopped
    private(set) var loadFraction = 0.0
    private(set) var loadMessage = "Loading"
    var message: Message?
    private(set) var controllerNames: [String] = []

    /// Dekui's menu is up. The game pauses while it is shown.
    var menuShown = false {
        didSet {
            guard menuShown != oldValue else { return }
            menuShown ? menuOpened() : menuClosed()
        }
    }
    /// Pages to open in the menu, e.g. when the game's overlay asks for Controls.
    var menuPath: [Page] = []
    /// Set while Controls waits for a button; the Siri Remote's Back cancels.
    @ObservationIgnored var captureCancel: (() -> Void)?

    @ObservationIgnored private var displayLink: CADisplayLink?
    @ObservationIgnored private var lastMenuButton = 0.0
    /// The game was paused by Dekui (menu, background), not by the player.
    @ObservationIgnored private var autoPaused = false
    @ObservationIgnored private let input = TVControllerInput()

    override init() {
        super.init()
        emulator.host = self
        input.model = self
        NotificationCenter.default.addObserver(
            self, selector: #selector(controllersChanged), name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(
            self, selector: #selector(controllersChanged), name: .GCControllerDidDisconnect, object: nil)
        controllersChanged()
    }

    var gameFolder: URL { emulator.gameFolderURL }
    var gameRunning: Bool { state == .running || state == .paused }

    // MARK: Setup

    func checkSetup() {
        tvLog("checking setup in \(gameFolder.path)")
        setup = .checking
        // Verifying the game reads the whole dump; keep it off the main thread.
        Task.detached(priority: .userInitiated) {
            try? FileManager.default.createDirectory(
                at: DKEmulator.shared.gameFolderURL, withIntermediateDirectories: true)
            var text: NSString?
            let status = DKEmulator.shared.prepare(withMessage: &text)
            let message = (text as String?) ?? ""
            tvLog("setup \(status.rawValue): \(message) home \(NSHomeDirectory()); iCloud signed in "
                + "\(FileManager.default.ubiquityIdentityToken != nil), container "
                + "\(FileManager.default.url(forUbiquityContainerIdentifier: nil)?.path ?? "none")")
            await MainActor.run {
                switch status {
                case .ready:
                    self.applyTVDefaults()
                    self.setup = .ready
                    self.startTicking()
                case .missingGame:
                    self.setup = .missingGame(message)
                default:
                    self.setup = .failed(message)
                }
            }
        }
    }

    /// The prepared user folder usually comes from the headset or a desktop,
    /// whose internal resolution is too much for an Apple TV GPU. Start at 2×
    /// once; the player can change it in Settings.
    private func applyTVDefaults() {
        let key = "DekuiTVDefaultsApplied"
        guard !UserDefaults.standard.bool(forKey: key) else { return }
        if emulator.resolution > 2 {
            emulator.resolution = 2
        }
        UserDefaults.standard.set(true, forKey: key)
    }

    // MARK: Emulation

    func start(layer: CAMetalLayer, size: CGSize) {
        guard setup == .ready, state == .stopped else { return }
        loadFraction = 0
        loadMessage = "Loading"
        emulator.start(with: layer, size: size)
    }

    func restart() {
        menuShown = false
        emulator.restart()
    }

    /// Stops emulation, saves settings and ends the app.
    func quit() {
        emulator.stop()
        exit(0)
    }

    func scenePhaseChanged(_ phase: ScenePhase) {
        tvLog("scene phase \(phase)")
        switch phase {
        case .active:
            if autoPaused && state == .paused && !menuShown {
                emulator.resume()
                autoPaused = false
            }
        case .background:
            if state == .running {
                tvLog("pausing for background")
                autoPaused = true
                emulator.pause()
            }
        default:
            break
        }
    }

    private func startTicking() {
        guard displayLink == nil else { return }
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    @objc private func tick() {
        emulator.tick()
    }

    /// `-DekuiDebugFrameEvery <seconds>`: writes the rendered frame to
    /// user/log/debug-frame.jpg every few seconds, for checking rendering
    /// remotely.
    @ObservationIgnored private var debugSnapshotScheduled = false
    private func scheduleDebugSnapshot() {
        let interval = UserDefaults.standard.double(forKey: "DekuiDebugFrameEvery")
        guard interval > 0, !debugSnapshotScheduled else { return }
        debugSnapshotScheduled = true
        Task { @MainActor in
            while true {
                try? await Task.sleep(for: .seconds(interval))
                tvLog("debug frame capture, state \(state.rawValue)")
                emulator.captureDebugFrame()
            }
        }
    }

    // MARK: Menu

    /// The menu button (Siri Remote Back or Play/Pause, controller L3+R3):
    /// closes Game Options when they are open, otherwise toggles the menu.
    /// One press can arrive both through UIKit (SwiftUI's exit and play/pause
    /// commands) and through the Game Controller framework; act on it once.
    func menuButtonPressed(from source: String) {
        let now = CACurrentMediaTime()
        guard now - lastMenuButton > 0.4 else { return }
        lastMenuButton = now
        tvLog("menu button (\(source)); game options \(emulator.dekuiSettingsVisible), menu \(menuShown)")
        if emulator.dekuiSettingsVisible {
            emulator.toggleDekuiSettings()
        } else if setup == .ready {
            menuShown.toggle()
        }
    }

    /// Closes the menu and shows Game Options over the running game.
    func openGameOptions() {
        menuShown = false
        if !emulator.dekuiSettingsVisible {
            emulator.toggleDekuiSettings()
        }
    }

    func open(_ page: Page) {
        menuPath = [page]
        menuShown = true
    }

    private func menuOpened() {
        tvLog("menu opened, path \(menuPath)")
        input.menuActive = true
        if state == .running {
            autoPaused = true
            emulator.pause()
        }
    }

    private func menuClosed() {
        tvLog("menu closed")
        input.menuActive = false
        menuPath = []
        if autoPaused && state == .paused {
            emulator.resume()
        }
        autoPaused = false
    }

    @objc private func controllersChanged() {
        controllerNames = GCController.controllers()
            .filter { $0.extendedGamepad != nil }
            .map { $0.vendorName ?? "Controller" }
        input.attach(GCController.controllers())
    }

    // MARK: DKEmulatorHost

    nonisolated func emulatorStateChanged(_ state: DKEmulationState) {
        MainActor.assumeIsolated {
            tvLog("emulation state \(state.rawValue)")
            self.state = state
            if state == .running { self.scheduleDebugSnapshot() }
        }
    }

    nonisolated func emulatorLoadProgress(_ fraction: Double, message: String) {
        MainActor.assumeIsolated {
            self.loadFraction = fraction
            self.loadMessage = message
        }
    }

    nonisolated func emulatorFailed(_ title: String, message: String) {
        MainActor.assumeIsolated { self.message = Message(title: title, text: message) }
    }

    nonisolated func emulatorShowControls() {
        MainActor.assumeIsolated { self.open(.controls) }
    }

    nonisolated func emulatorShowLicenses(_ path: String) {
        MainActor.assumeIsolated { self.open(.licenses) }
    }

    nonisolated func emulatorOpenTextureFolder(_ path: String) {
        MainActor.assumeIsolated {
            self.message = Message(
                title: "Custom textures",
                text: "Copy texture packs to the Apple TV with tools/tvos/copy-game.sh; they go in "
                    + "user/load/textures/0004000000125500.")
        }
    }

    nonisolated func emulatorQuit() {
        MainActor.assumeIsolated { self.quit() }
    }
}

/// Reads the buttons Dekui itself uses from the Game Controller framework.
/// The game reads controllers separately, through SDL.
@MainActor
final class TVControllerInput {
    weak var model: TVModel?
    /// Dekui's menu has focus: UIKit handles the remote and controllers.
    var menuActive = false {
        didSet { if menuActive != oldValue { menuChanged = CACurrentMediaTime() } }
    }
    /// A press that closes the menu through UIKit also reaches the handlers
    /// below; ignore menu presses right after the menu opened or closed.
    private var menuChanged = 0.0
    private var attached = Set<ObjectIdentifier>()

    func attach(_ controllers: [GCController]) {
        for controller in controllers where !attached.contains(ObjectIdentifier(controller)) {
            attached.insert(ObjectIdentifier(controller))
            tvLog("controller \(controller.vendorName ?? "?"): extended \(controller.extendedGamepad != nil), "
                + "micro \(controller.microGamepad != nil), category \(controller.productCategory)")
            if let gamepad = controller.extendedGamepad {
                attachGamepad(gamepad)
            } else if let remote = controller.microGamepad {
                attachRemote(remote)
            }
        }
    }

    private func attachGamepad(_ gamepad: GCExtendedGamepad) {
        if let home = gamepad.buttonHome {
            // Deliver short Home presses to Dekui; a long press still opens
            // the system's Control Center.
            home.preferredSystemGestureState = .disabled
            home.pressedChangedHandler = { [weak self] _, _, pressed in
                if pressed { self?.menuButton("controller Home") }
            }
        }
        // Both stick clicks together (unbound by the automatic mapping) open
        // the menu: tvOS 26 keeps the Home button for Control Center.
        if let left = gamepad.leftThumbstickButton, let right = gamepad.rightThumbstickButton {
            let chord: GCControllerButtonValueChangedHandler = { [weak self] _, _, _ in
                if left.isPressed && right.isPressed { self?.menuButton("controller L3+R3") }
            }
            left.pressedChangedHandler = chord
            right.pressedChangedHandler = chord
        }
        navigate(gamepad.dpad.up, .up)
        navigate(gamepad.dpad.down, .down)
        navigate(gamepad.dpad.left, .left)
        navigate(gamepad.dpad.right, .right)
        navigate(gamepad.leftThumbstick.up, .up)
        navigate(gamepad.leftThumbstick.down, .down)
        navigate(gamepad.leftThumbstick.left, .left)
        navigate(gamepad.leftThumbstick.right, .right)
        navigate(gamepad.buttonA, .activate)
        navigate(gamepad.buttonB, .cancel)
        // Diagnostics: Start reaches the game through SDL; log what tvOS does with it.
        let menu = gamepad.buttonMenu
        menu.pressedChangedHandler = { _, _, pressed in
            tvLog("controller Start \(pressed ? "down" : "up")")
        }
        navigate(gamepad.leftShoulder, .previous)
        navigate(gamepad.rightShoulder, .next)
    }

    private func attachRemote(_ remote: GCMicroGamepad) {
        remote.reportsAbsoluteDpadValues = false
        remote.allowsRotation = false
        let name = remote.controller?.vendorName ?? "?"
        remote.buttonMenu.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.menuButton("\(name) Menu") }
        }
        // Play/Pause.
        remote.buttonX.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.menuButton("\(name) Play/Pause") }
        }
        navigate(remote.dpad.up, .up)
        navigate(remote.dpad.down, .down)
        navigate(remote.dpad.left, .left)
        navigate(remote.dpad.right, .right)
        navigate(remote.buttonA, .activate)
    }

    private func menuButton(_ source: String) {
        if let cancel = model?.captureCancel {
            cancel()
            return
        }
        guard let model, !menuActive, CACurrentMediaTime() - menuChanged > 0.4 else { return }
        model.menuButtonPressed(from: source)
    }

    /// Forwards a button to Game Options while they are open.
    private func navigate(_ button: GCControllerButtonInput, _ input: DKOverlayInput) {
        button.pressedChangedHandler = { [weak self] _, _, pressed in
            guard let self, let model = self.model, !self.menuActive else { return }
            if !pressed || model.emulator.dekuiSettingsVisible {
                model.emulator.overlayInput(input, down: pressed)
            }
        }
    }
}
