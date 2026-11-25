// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import GameController
import SwiftUI
import UIKit

/// A diagnostic line in the device console and, once the emulator log is
/// open, in user/log.
func tvLog(_ message: String) {
    print("[DekuiTV] \(message)")
    DKEmulator.shared.logMessage("tvOS: \(message)")
}

/// The emulator's render target.
final class TVGameHostView: UIView {
    /// A sublayer rather than the view's backing layer: MoltenVK configures the
    /// layer from the emulation thread, which UIKit forbids for backing layers.
    let metalLayer = CAMetalLayer()
    var onLayout: ((CAMetalLayer, CGSize) -> Void)?
    private let emulator = DKEmulator.shared
    private var loggedSize = CGSize.zero

    override init(frame: CGRect) {
        super.init(frame: frame)
        layer.addSublayer(metalLayer)
        layer.masksToBounds = true
        // One pixel per point: the 3DS image is already upscaled, and the TV
        // scales the 1080p image to its panel.
        metalLayer.contentsScale = 1
        metalLayer.framebufferOnly = true
        backgroundColor = .black
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    /// The game view takes the focus, so presses reach its
    /// GCEventViewController, which keeps them from UIKit.
    override var canBecomeFocused: Bool { true }

    override func layoutSubviews() {
        super.layoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        metalLayer.frame = bounds
        metalLayer.contentsScale = 1
        CATransaction.commit()
        guard bounds.width > 0, bounds.height > 0 else { return }
        if bounds.size != loggedSize {
            loggedSize = bounds.size
            tvLog("game view \(bounds.size), window \(window?.bounds.size ?? .zero), "
                + "layer \(metalLayer.frame.size) scale \(metalLayer.contentsScale)")
        }
        emulator.setViewSize(bounds.size)
        onLayout?(metalLayer, bounds.size)
    }
}

/// Hosts the game view. While it has focus, the Siri Remote and controllers
/// go only to the Game Controller framework (the game and Dekui's menu
/// button), not to UIKit, so Back does not leave the app.
final class TVGameViewController: GCEventViewController {
    let hostView = TVGameHostView(frame: .zero)

    override func loadView() {
        view = hostView
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        controllerUserInteractionEnabled = false
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        tvLog("game controller appeared; first responder \(isFirstResponder), "
            + "parent \(String(describing: parent.map { type(of: $0) }))")
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        tvLog("UIKit press began: \(presses.map { $0.type.rawValue })")
        super.pressesBegan(presses, with: event)
    }
}

struct TVGameView: UIViewControllerRepresentable {
    let model: TVModel

    func makeUIViewController(context: Context) -> TVGameViewController {
        let controller = TVGameViewController()
        controller.hostView.onLayout = { layer, size in
            MainActor.assumeIsolated { model.start(layer: layer, size: size) }
        }
        return controller
    }

    func updateUIViewController(_ controller: TVGameViewController, context: Context) {}
}

/// The game, full screen, with a loading indicator until the first frame.
struct TVGameScreen: View {
    @Environment(TVModel.self) private var model

    var body: some View {
        ZStack {
            TVGameView(model: model)
                .ignoresSafeArea()
            if model.state == .loading {
                VStack(spacing: 24) {
                    ProgressView(value: model.loadFraction)
                        .frame(width: 600)
                    Text(model.loadMessage)
                        .font(.headline)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 800)
                }
                .padding(48)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 24))
            }
        }
        // Fallbacks for presses that reach UIKit anyway: Back must open
        // Dekui's menu instead of leaving the app.
        .onExitCommand { model.menuButtonPressed(from: "exit command") }
        .onPlayPauseCommand { model.menuButtonPressed(from: "play/pause command") }
        .onChange(of: model.state) { old, state in
            // `-DekuiShowGameOptions YES` opens Game Options once the game
            // runs, and `-DekuiOpenMenu YES` Dekui's menu, for screenshots.
            guard old == .loading, state == .running else { return }
            if UserDefaults.standard.bool(forKey: "DekuiShowGameOptions") {
                model.openGameOptions()
            } else if UserDefaults.standard.bool(forKey: "DekuiOpenMenu") {
                model.menuShown = true
            }
        }
    }
}
