// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GameController
import Observation
import QuartzCore
import simd
import SwiftUI
import UIKit

/// App-wide state shared by the game, settings and controls windows.
@MainActor
@Observable
final class EmulatorModel: NSObject, DKEmulatorHost {
    enum Setup: Equatable {
        case checking
        case ready
        case missingGame(String)
        case failed(String)
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
    /// Incremented when the overlay asks for a window; views open it.
    private(set) var controlsRequests = 0
    private(set) var licensesRequests = 0
    private(set) var controllerNames: [String] = []
    /// Stereo 3D is on: frames go to `stereoScreen` instead of the game view's layer.
    private(set) var stereoscopic = false
    @ObservationIgnored let stereoScreen = StereoScreen()

    /// The game window's ornament. While the game runs it fades out after a
    /// few seconds; a pinch on the game brings it back.
    private(set) var ornamentVisible = true
    var autoHideOrnament = UserDefaults.standard.object(forKey: "DekuiAutoHideControls") as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(autoHideOrnament, forKey: "DekuiAutoHideControls")
            showOrnament()
        }
    }
    /// The game window is enlarged with its window bar hidden.
    private(set) var fullScreen = false
    /// The player asked for the room around the game window to be dimmed.
    var dimRequested = false {
        didSet { if dimRequested != oldValue { immersiveSpaceRequests += 1 } }
    }
    /// Dekui's immersive space is open: for dimming, head tracking or both.
    private(set) var immersiveSpaceOpen = false
    /// The room around the game window is dimmed.
    var surroundingsDimmed: Bool { immersiveSpaceOpen && dimRequested }
    /// Incremented when the immersive space should open or close; the game
    /// window's root view does it (it has the scene actions).
    private(set) var immersiveSpaceRequests = 0
    var immersiveSpaceWanted: Bool { dimRequested || motionSource == .headTracking || wideView }

    /// Immersive wide view (prototype): the game draws a wide field of view on
    /// a world-locked curved surface in the immersive space.
    var wideView = false {
        didSet {
            guard wideView != oldValue else { return }
            emulator.wideView = wideView
            if wideView {
                wideScreen.recenter()
            } else {
                hudAdjusting = false
                wideScreen.hide()
            }
            compactGameWindow(wideView)
            updateHeadTracking()
            immersiveSpaceRequests += 1
        }
    }
    /// Vertical field of view of the standard gameplay camera in the wide view.
    var wideViewFov = UserDefaults.standard.object(forKey: "DekuiWideViewFov") as? Int ?? 100 {
        didSet {
            UserDefaults.standard.set(wideViewFov, forKey: "DekuiWideViewFov")
            emulator.wideViewFov = wideViewFov
        }
    }
    /// Percent of the way from a true horizon (0) to a centred character (100).
    var wideViewCentring = UserDefaults.standard.object(forKey: "DekuiWideViewCentring") as? Int ?? 50 {
        didSet {
            UserDefaults.standard.set(wideViewCentring, forKey: "DekuiWideViewCentring")
            emulator.wideViewCentring = Float(wideViewCentring) / 100
        }
    }
    var immersiveUpscaling = (UserDefaults.standard.object(
        forKey: "DekuiImmersiveUpscaling") as? Int).flatMap(ImmersiveUpscaling.init(rawValue:))
        ?? .oneAndAHalf {
        didSet {
            UserDefaults.standard.set(immersiveUpscaling.rawValue,
                                      forKey: "DekuiImmersiveUpscaling")
            wideScreen.upscaling = immersiveUpscaling
        }
    }
    var immersiveAntialiasing = UserDefaults.standard.object(
        forKey: "DekuiImmersiveAntialiasing") as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(immersiveAntialiasing,
                                      forKey: "DekuiImmersiveAntialiasing")
            wideScreen.antialiasing = immersiveAntialiasing
        }
    }
    @ObservationIgnored let wideScreen = WideScreen()
    /// The player moved or scaled the immersive view's HUD by hand.
    private(set) var hudAdjusted = WideHud.Adjustment.load() != WideHud.Adjustment()
    /// HUD adjustment mode: pinches on the HUD move and scale it instead of
    /// going to the game. Ends with Done, or when the controls fade.
    var hudAdjusting = false {
        didSet {
            guard hudAdjusting != oldValue else { return }
            wideScreen.hud.adjustable = hudAdjusting
            showOrnament()
        }
    }
    /// Incremented by Recenter; the immersive view's hint asks the player to
    /// look straight ahead.
    private(set) var recenterCount = 0
    /// The room's tint around the immersive view: the game image's average
    /// colour, darkened (linear RGB), as the Photos app does for its
    /// immersive pictures. Eased, and published a few times a second.
    private(set) var surroundingsTint: SIMD3<Float>?
    @ObservationIgnored private var easedTint: SIMD3<Float>?
    @ObservationIgnored private var tintPublished: CFTimeInterval = 0
    @ObservationIgnored private var sizeBeforeWideView: CGSize?

    /// What drives the 3DS gyroscope and accelerometer; saved to qt-config.ini.
    var motionSource: DKMotionSource = .controller {
        didSet {
            guard motionSource != oldValue else { return }
            DKMotion.source = motionSource
            updateHeadTracking()
            immersiveSpaceRequests += 1
        }
    }
    /// Head tracking is selected but ARKit has no immersive space to report in,
    /// e.g. after the space was closed with the Digital Crown.
    var headTrackingPaused: Bool { motionSource == .headTracking && !immersiveSpaceOpen }
    @ObservationIgnored private let headTracker = HeadTracker()
    /// The game window's scene, for resizing it.
    @ObservationIgnored weak var gameScene: UIWindowScene?
    @ObservationIgnored private var windowedSize: CGSize?
    @ObservationIgnored private var windowResizeTask: Task<Void, Never>?
    @ObservationIgnored private var ornamentHideTask: Task<Void, Never>?
    /// Reasons to keep the ornament up, such as an open confirmation dialog.
    @ObservationIgnored private var ornamentHolds = 0

    private var displayLink: CADisplayLink?
    private var autoPaused = false
    @ObservationIgnored private var overlayButtonStates = [Bool](repeating: false, count: 8)

    override init() {
        super.init()
        emulator.host = self
        emulator.wideViewFov = wideViewFov
        emulator.wideViewCentring = Float(wideViewCentring) / 100
        wideScreen.upscaling = immersiveUpscaling
        wideScreen.antialiasing = immersiveAntialiasing
        NotificationCenter.default.addObserver(
            self, selector: #selector(controllersChanged), name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(
            self, selector: #selector(controllersChanged), name: .GCControllerDidDisconnect, object: nil)
        controllersChanged()
    }

    var gameFolder: URL { emulator.gameFolderURL }

    // MARK: Setup

    func checkSetup() {
        setup = .checking
        // Verifying the game reads the whole dump; keep it off the main thread.
        Task.detached(priority: .userInitiated) {
            var text: NSString?
            let status = DKEmulator.shared.prepare(withMessage: &text)
            let message = (text as String?) ?? ""
            await MainActor.run {
                switch status {
                case .ready:
                    self.setup = .ready
                    // Settings are loaded now. Not through the setter: nothing changed.
                    self.loadMotionSource()
                    self.startTicking()
                case .missingGame:
                    self.setup = .missingGame(message)
                default:
                    self.setup = .failed(message)
                }
            }
        }
    }

    /// Copies a game dump picked in Files into the Dekui folder.
    func importGame(from url: URL) {
        importItem(from: url, to: gameFolder.appendingPathComponent(url.lastPathComponent))
    }

    /// Copies a desktop-prepared user folder into the Dekui folder.
    func importUserFolder(from url: URL) {
        importItem(from: url, to: gameFolder.appendingPathComponent("user", isDirectory: true))
    }

    private func importItem(from url: URL, to destination: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let manager = FileManager.default
            if manager.fileExists(atPath: destination.path) {
                try manager.removeItem(at: destination)
            }
            try manager.copyItem(at: url, to: destination)
            checkSetup()
        } catch {
            message = Message(title: "Import failed", text: error.localizedDescription)
        }
    }

    // MARK: Emulation

    func start(layer: CAMetalLayer, size: CGSize) {
        guard setup == .ready, state == .stopped else { return }
        loadFraction = 0
        loadMessage = "Loading"
        emulator.start(with: layer, size: size)
    }

    /// Stops emulation, saves settings and ends the app. visionOS itself only
    /// offers Force Quit, which skips the settings save.
    func quit() {
        emulator.stop()
        exit(0)
    }

    /// Reboots the game, as turning the 3DS off and on would.
    func restart() {
        emulator.restart()
    }

    func togglePause() {
        switch state {
        case .running: emulator.pause()
        case .paused: emulator.resume()
        default: break
        }
    }

    /// Pauses while the game window is in the background, and resumes only a
    /// pause made here, not one the player chose.
    func scenePhaseChanged(_ phase: ScenePhase) {
        switch phase {
        case .active:
            if autoPaused && state == .paused {
                emulator.resume()
            }
            autoPaused = false
        case .background:
            if state == .running {
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
        updateOverlayController()
        if wideView {
            // The immersive space takes every frame, in 2D or 3D.
            if immersiveSpaceOpen {
                wideScreen.update(from: emulator, head: headTracker.pose)
                updateSurroundingsTint()
            }
            return
        }
        let stereo = emulator.stereoscopic
        if stereo != stereoscopic {
            stereoscopic = stereo
            stereoScreen.hide()
        }
        if stereo {
            stereoScreen.update(from: emulator)
        }
    }

    func toggleStereoscopic() {
        emulator.stereoscopic = !emulator.stereoscopic
    }

    // MARK: Game window

    /// Shows the ornament and restarts its hide timer.
    func showOrnament() {
        if !ornamentVisible {
            withAnimation { ornamentVisible = true }
        }
        scheduleOrnamentHide()
    }

    /// A pinch began on the game. Returns true when it only brought the
    /// ornament back and should not reach the game's overlay.
    func gamePinched() -> Bool {
        let revealed = !ornamentVisible
        showOrnament()
        return revealed && !emulator.dekuiSettingsVisible
    }

    /// Keeps the ornament up while `held`, e.g. while one of its dialogs is open.
    func holdOrnament(_ held: Bool) {
        ornamentHolds = max(0, ornamentHolds + (held ? 1 : -1))
        showOrnament()
    }

    private func scheduleOrnamentHide() {
        ornamentHideTask?.cancel()
        ornamentHideTask = nil
        guard autoHideOrnament || fullScreen || wideView, state == .running,
              ornamentHolds == 0 else { return }
        ornamentHideTask = Task { [weak self] in
            // Longer while adjusting the HUD; each gesture restarts it.
            try? await Task.sleep(for: .seconds(self?.hudAdjusting == true ? 10 : 4))
            guard !Task.isCancelled, let self,
                  self.state == .running, self.ornamentHolds == 0 else { return }
            withAnimation(.easeOut(duration: 0.4)) { self.ornamentVisible = false }
            self.hudAdjusting = false
        }
    }

    /// Shrinks the game window while the wide view shows the game, so it does
    /// not cover the immersive screen; restores it after.
    private func compactGameWindow(_ compact: Bool) {
        guard let scene = gameScene else { return }
        let size: CGSize
        if compact {
            sizeBeforeWideView = fullScreen
                ? windowedSize
                : scene.coordinateSpace.bounds.size
            fullScreen = false
            // RootView's minimum size.
            size = CGSize(width: 640, height: 360)
        } else {
            // The game keeps 16:9; restore that shape whatever was measured.
            let width = max(640, sizeBeforeWideView?.width ?? 1280)
            size = CGSize(width: width, height: (width * 9 / 16).rounded())
            sizeBeforeWideView = nil
        }
        resizeGameWindow(scene, to: size)
        showOrnament()
    }

    /// Enlarges the game window and hides its window bar, or restores it.
    func toggleFullScreen() {
        guard let scene = gameScene else { return }
        let size: CGSize
        if fullScreen {
            let width = max(640, windowedSize?.width ?? 1280)
            size = CGSize(width: width, height: (width * 9 / 16).rounded())
        } else {
            windowedSize = scene.keyWindow?.bounds.size ?? scene.windows.first?.bounds.size
            // visionOS caps the size at the largest window it allows.
            size = CGSize(width: 3200, height: 1800)
        }
        fullScreen.toggle()
        resizeGameWindow(scene, to: size)
        showOrnament()
    }

    /// Resizes the game window and keeps it 16:9. With uniform resizing on,
    /// visionOS scales the window's current shape rather than taking the
    /// requested one, and it caps each side of an oversized request on its
    /// own, so a 16:9 request can come out another shape. Resize freely, trim
    /// the result to 16:9, then lock uniform resizing again.
    private func resizeGameWindow(_ scene: UIWindowScene, to size: CGSize) {
        windowResizeTask?.cancel()
        scene.requestGeometryUpdate(.Vision(size: size, resizingRestrictions: .freeform)) { error in
            print("Could not resize the game window: \(error)")
        }
        windowResizeTask = Task { [weak scene] in
            try? await Task.sleep(for: .milliseconds(500))
            guard !Task.isCancelled, let scene else { return }
            let actual = scene.coordinateSpace.bounds.size
            var fitted = size
            if actual.width > 0, actual.height > 0 {
                let width = min(actual.width, actual.height * 16 / 9).rounded()
                fitted = CGSize(width: width, height: (width * 9 / 16).rounded())
            }
            scene.requestGeometryUpdate(.Vision(size: fitted, resizingRestrictions: .uniform)) { error in
                print("Could not lock the game window's aspect ratio: \(error)")
            }
        }
    }

    private func updateSurroundingsTint() {
        guard let colour = wideScreen.averageColor else { return }
        // colorMultiply is substantially darker than Photos' luminous spatial
        // picture surround. Preserve more of the sampled colour and keep dim
        // scenes from turning the feather into a grey/black vignette.
        let target = simd_clamp(colour * 0.72, SIMD3(repeating: 0.08), SIMD3(repeating: 0.78))
        let eased = easedTint.map { $0 + (target - $0) * 0.05 } ?? target
        easedTint = eased
        let now = CACurrentMediaTime()
        guard now - tintPublished > 0.2 else { return }
        if let shown = surroundingsTint, simd_length(shown - eased) < 0.01 { return }
        tintPublished = now
        surroundingsTint = eased
    }

    // MARK: Motion controls

    private func loadMotionSource() {
        let saved = DKMotion.source
        if saved != motionSource {
            // Assigning saves the setting again, which is harmless.
            motionSource = saved
        }
    }

    func immersiveSpaceOpened() {
        immersiveSpaceOpen = true
        updateHeadTracking()
    }

    func immersiveSpaceClosed() {
        immersiveSpaceOpen = false
        // Closed by the system (Digital Crown) or by Dekui: nothing to dim
        // now, and the wide view has nowhere to show.
        if dimRequested {
            dimRequested = false
        }
        if wideView {
            wideView = false
        }
        updateHeadTracking()
    }

    /// Opening the immersive space failed.
    func immersiveSpaceFailed() {
        dimRequested = false
        if motionSource == .headTracking {
            message = Message(title: "Head tracking is unavailable",
                              text: "Dekui could not open the space that head tracking needs. "
                                  + "Close other immersive apps and try again.")
        }
    }

    /// Opens the immersive space again for head tracking.
    func resumeHeadTracking() {
        immersiveSpaceRequests += 1
    }

    func recenterHeadTracking() {
        DKMotion.recenterHead()
        wideScreen.recenter()
        recenterCount += 1
    }

    /// Saves the HUD's adjustment after a gesture.
    func hudAdjustmentChanged() {
        let adjustment = wideScreen.hud.adjustment
        adjustment.save()
        hudAdjusted = adjustment != WideHud.Adjustment()
    }

    /// Back to the automatic placement and size.
    func resetHud() {
        wideScreen.hud.adjustment = WideHud.Adjustment()
        hudAdjustmentChanged()
    }

    private func updateHeadTracking() {
        headTracker.feedsMotion = motionSource == .headTracking
        if immersiveSpaceOpen && (motionSource == .headTracking || wideView) {
            headTracker.start()
        } else {
            headTracker.stop()
        }
    }

    @objc private func controllersChanged() {
        controllerNames = GCController.controllers().map { $0.vendorName ?? "Controller" }
    }

    /// Poll instead of replacing GameController callbacks owned by SDL. Merge
    /// D-pad, stick and multiple controllers before sending key transitions so
    /// releasing one source does not cancel another source that is still held.
    private func updateOverlayController() {
        let inputs: [DKOverlayInput] = [.up, .down, .left, .right,
                                        .activate, .cancel, .previous, .next]
        var pressed = [Bool](repeating: false, count: inputs.count)
        if emulator.dekuiSettingsVisible {
            for controller in GCController.controllers() {
                guard let pad = controller.extendedGamepad else { continue }
                let state = [
                    pad.dpad.up.isPressed || pad.leftThumbstick.yAxis.value > 0.5,
                    pad.dpad.down.isPressed || pad.leftThumbstick.yAxis.value < -0.5,
                    pad.dpad.left.isPressed || pad.leftThumbstick.xAxis.value < -0.5,
                    pad.dpad.right.isPressed || pad.leftThumbstick.xAxis.value > 0.5,
                    pad.buttonA.isPressed, pad.buttonB.isPressed,
                    pad.leftShoulder.isPressed, pad.rightShoulder.isPressed,
                ]
                for index in pressed.indices { pressed[index] = pressed[index] || state[index] }
            }
        }
        for index in inputs.indices where pressed[index] != overlayButtonStates[index] {
            emulator.overlayInput(inputs[index], down: pressed[index])
        }
        overlayButtonStates = pressed
    }

    // MARK: DKEmulatorHost

    nonisolated func emulatorStateChanged(_ state: DKEmulationState) {
        MainActor.assumeIsolated {
            self.state = state
            self.showOrnament()
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
        MainActor.assumeIsolated { self.controlsRequests += 1 }
    }

    nonisolated func emulatorShowLicenses(_ path: String) {
        MainActor.assumeIsolated { self.licensesRequests += 1 }
    }

    nonisolated func emulatorOpenTextureFolder(_ path: String) {
        MainActor.assumeIsolated {
            self.message = Message(
                title: "Custom textures",
                text: "Put texture packs in Files › On My Apple Vision Pro › Dekui › "
                    + "user › load › textures › 0004000000125500.")
        }
    }

    nonisolated func emulatorQuit() {
        // The overlay's Quit button already asked for confirmation.
        MainActor.assumeIsolated { self.quit() }
    }
}
