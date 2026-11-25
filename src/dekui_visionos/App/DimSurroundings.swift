// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import RealityKit
import SwiftUI

/// Dekui's immersive space: it dims the room around the game window, lets
/// ARKit report the head pose for head-tracked motion controls, and holds the
/// immersive wide view's curved screen. Opening any immersive space replaces
/// a system environment, so this is offered instead of an environment of
/// Dekui's own.
struct DimSurroundings: View {
    static let id = "dim-surroundings"

    @Environment(EmulatorModel.self) private var model
    /// The HUD's adjustment and the pointer's direction when a gesture began.
    @State private var dragStart: (adjustment: WideHud.Adjustment, angles: SIMD2<Float>)?
    @State private var magnifyStart: WideHud.Adjustment?

    /// The immersive view tints the room with the game's colour (as Photos
    /// does for immersive pictures); dimming alone darkens it.
    private var surroundings: SurroundingsEffect? {
        if model.wideView, let tint = model.surroundingsTint {
            return .colorMultiply(Color(.sRGBLinear, red: Double(tint.x), green: Double(tint.y),
                                        blue: Double(tint.z)))
        }
        return model.dimRequested || model.wideView ? .systemDark : nil
    }

    var body: some View {
        // The immersive wide view's curved screen; empty unless it is on.
        RealityView { content in
            await model.wideScreen.loadMaterial()
            content.add(model.wideScreen.root)
        }
        .gesture(moveHud)
        .simultaneousGesture(scaleHud)
        .preferredSurroundingsEffect(surroundings)
        .onAppear { model.immersiveSpaceOpened() }
        .onDisappear { model.immersiveSpaceClosed() }
    }

    /// Pinch and drag the HUD to move it around the head.
    private var moveHud: some Gesture {
        DragGesture(minimumDistance: 0)
            .targetedToEntity(model.wideScreen.hud.entity)
            .onChanged { value in
                // Pinching with both hands scales it instead.
                guard magnifyStart == nil else { return }
                model.showOrnament()
                let hud = model.wideScreen.hud
                let point = value.convert(value.location3D, from: .local,
                                          to: model.wideScreen.root)
                let angles = WideHud.angles(of: point)
                if dragStart == nil {
                    dragStart = (hud.adjustment, angles)
                }
                guard let start = dragStart else { return }
                var adjustment = start.adjustment
                adjustment.yaw += WideScreen.wrap(angles.x - start.angles.x)
                adjustment.pitch += angles.y - start.angles.y
                hud.adjustment = adjustment
            }
            .onEnded { _ in
                dragStart = nil
                model.hudAdjustmentChanged()
            }
    }

    /// Pinch the HUD with both hands to scale it.
    private var scaleHud: some Gesture {
        MagnifyGesture()
            .targetedToEntity(model.wideScreen.hud.entity)
            .onChanged { value in
                let hud = model.wideScreen.hud
                model.showOrnament()
                if magnifyStart == nil {
                    magnifyStart = hud.adjustment
                    dragStart = nil
                }
                guard var adjustment = magnifyStart else { return }
                adjustment.scale *= Float(value.magnification)
                hud.adjustment = adjustment
            }
            .onEnded { _ in
                magnifyStart = nil
                model.hudAdjustmentChanged()
            }
    }
}
