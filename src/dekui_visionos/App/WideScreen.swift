// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Metal
import QuartzCore
import RealityKit
import simd

/// Immersive wide view (prototype). The game renders a wide field of view and
/// each frame arrives with the game camera that drew it. The frame is shown on
/// a section of a sphere around the player's head, covering exactly the angles
/// the frame's projection covers. Between game frames the image stays put in
/// the room while the head moves (visionOS draws it at the display rate).
///
/// - Third person: head look. The head's turn since the recentre turns the
///   game's view in place (DKEmulator setHeadLook, applied where the game
///   builds its view), and each frame is placed at the heading its view was
///   turned to.
/// - First person: gyro aiming turns the camera with the head; the frame is
///   placed at the camera's heading plus a fixed offset into the room.
///
/// The game camera's centre, where the character is, sits where the head
/// pointed at the recentre: the head's raise since then is added to the
/// camera's own pitch, as its turn is to the camera's heading, so the
/// character stays centred however the camera orbits or cuts (the game's
/// horizon tilts with the camera, as on a 3DS).
@MainActor
final class WideScreen {
    let root = Entity()
    private let screen = ModelEntity(mesh: .generatePlane(width: 1, height: 1))
    private var material: ShaderGraphMaterial?
    private var texture: LowLevelTexture?
    private var queue: MTLCommandQueue?
    private var eyes = 0
    private let upscaler = ImmersiveUpscaler()
    private let halo = WideHalo()
    let hud = WideHud()

    var upscaling: ImmersiveUpscaling = .oneAndAHalf
    var antialiasing = true

    /// Metres from the head to the image.
    private let radius: Float = 4
    /// A larger first-person yaw change between two frames is a camera cut.
    private let cutRadians: Float = 0.8
    /// The furthest the head look raises or lowers the view.
    private let maxPitch: Float = 1.2

    private var tangents = SIMD2<Float>.zero
    /// The shown frame's centre: its pitch in the room (up positive).
    private var screenPitch: Float = 0
    /// Third person: where the game camera's own view is in the room, for
    /// the HUD (the recentre's pitch).
    private var hudPitch: Float?
    /// The game camera's own pitch before the head look, for the log.
    private var basePitch: Float?
    /// Where the head was at the recentre.
    private var headPosition = SIMD3<Float>.zero
    /// When a requested recentre happens.
    private var recenterAt: CFTimeInterval?
    private let recenterDelay: CFTimeInterval = 1.0
    private var placed = false
    private var recenterRequested = true

    /// The room heading and pitch that count as straight ahead in the game.
    private var anchorYaw: Float = 0
    private var anchorPitch: Float = 0
    /// The shown frame's heading in the room.
    private var screenYaw: Float?
    /// First person: room heading minus camera heading, and the last camera heading.
    private var firstPersonOffset: Float?
    private var lastCameraYaw: Float?
    private var lastLog: CFTimeInterval = 0
    private var droppedFrames = 0
    /// The newest frame is a full-screen menu (DKStereoFrame isMenu), and the angles of
    /// the flat panel the screen's mesh was built as for it.
    private var menu = false
    private var menuTangents: SIMD2<Float>?
    /// Unturned frames skipped in a row (see update).
    private var unturnedSkips = 0

    init() {
        screen.isEnabled = false
        root.addChild(halo.farEntity)
        root.addChild(halo.entity)
        root.addChild(screen)
        root.addChild(hud.anchor)
        // Far glow, near glow, then the screen, whose feathered edge blends over them.
        let group = ModelSortGroup(depthPass: nil)
        halo.farEntity.components.set(ModelSortGroupComponent(group: group, order: 0))
        halo.entity.components.set(ModelSortGroupComponent(group: group, order: 1))
        screen.components.set(ModelSortGroupComponent(group: group, order: 2))
        hud.entity.components.set(ModelSortGroupComponent(group: group, order: 3))
    }

    func loadMaterial() async {
        DKEmulator.shared.logWideView("loading materials")
        await halo.loadMaterial()
        await hud.loadMaterial()
        defer { DKEmulator.shared.logWideView("materials loaded: \(material != nil)") }
        guard material == nil else { return }
        do {
            var loaded = try await ShaderGraphMaterial(
                named: "/Root/WideScreen", from: Data(Self.materialSource.utf8))
            loaded.faceCulling = .none
            material = loaded
        } catch {
            // Without the edge fade rather than without a screen.
            NSLog("Dekui: the wide view material failed to load: %@", "\(error)")
            DKEmulator.shared.logWideView("edge fade material failed: \(error)")
            do {
                var plain = try await ShaderGraphMaterial(
                    named: "/Root/StereoScreen", from: Data(StereoScreen.materialSource.utf8))
                plain.faceCulling = .none
                material = plain
            } catch {
                NSLog("Dekui: the stereo material failed to load: %@", "\(error)")
                DKEmulator.shared.logWideView("stereo material failed: \(error)")
            }
        }
    }

    /// Hidden until the next frame, so stale frames never show.
    func hide() {
        screen.isEnabled = false
        halo.hide()
        hud.hide()
        screenYaw = nil
        hudPitch = nil
        basePitch = nil
        firstPersonOffset = nil
        lastCameraYaw = nil
        placed = false
    }

    /// The game image's average colour (linear RGB), for tinting the room.
    var averageColor: SIMD3<Float>? { halo.averageColor }

    /// Puts the view in front of the head again, where the head points a
    /// moment from now: the Recenter button itself is not straight ahead.
    func recenter() {
        recenterAt = CACurrentMediaTime() + recenterDelay
    }

    /// Call once per display frame with the newest head pose (world from device).
    func update(from emulator: DKEmulator, head: simd_float4x4?) {
        var headYaw: Float?
        var headPitch: Float = 0
        if let head {
            let forward = -SIMD3(head.columns.2.x, head.columns.2.y, head.columns.2.z)
            headYaw = Self.yaw(forward: forward)
            headPitch = asin(max(-1, min(1, forward.y)))
            if let at = recenterAt, CACurrentMediaTime() >= at {
                recenterAt = nil
                recenterRequested = true
            }
            if recenterRequested || !placed {
                headPosition = SIMD3(head.columns.3.x, head.columns.3.y, head.columns.3.z)
                anchorYaw = headYaw!
                anchorPitch = headPitch
                hudPitch = nil
                firstPersonOffset = nil
                recenterRequested = false
                placed = true
            }
        }

        // Third person: turn the game's view with the head.
        let firstPerson = emulator.firstPersonCamera
        if let headYaw, placed, !firstPerson {
            // Turn and raise from the recentre, added to the camera's own.
            emulator.setHeadLook(yaw: Self.wrap(headYaw - anchorYaw),
                                 pitch: max(-maxPitch, min(maxPitch, headPitch - anchorPitch)),
                                 anchorPitch: anchorPitch, enabled: true)
        } else {
            emulator.setHeadLook(yaw: 0, pitch: 0, anchorPitch: anchorPitch, enabled: false)
        }

        if let frame = emulator.acquireStereoFrame() {
            menu = frame.isMenu
            if menu {
                // Shown flat at the HUD's size, not at its camera's angles.
                unturnedSkips = 0
                if tangents == .zero {
                    let tanY = frame.hasCamera ? frame.tanHalfHeight
                        : tan(Float(emulator.wideViewFov) * .pi / 360)
                    resize(tangents: SIMD2(tanY * 5 / 3, tanY))
                }
                show(frame, emulator: emulator)
            } else if frame.isUnturned && unturnedSkips < 3 {
                // Drawn without the head look, it would put the screen back at
                // the recentre direction for one frame, showing the room. Keep
                // the previous frame instead, but never for long: a run of them
                // means the game's view really is unturned.
                unturnedSkips += 1
                emulator.releaseStereoFrame(frame)
            } else {
                unturnedSkips = 0
                if frame.hasCamera {
                    apply(frame, headYaw: headYaw, headPitch: headPitch,
                          firstPerson: firstPerson)
                    resize(tangents: SIMD2(frame.tanHalfWidth, frame.tanHalfHeight))
                }
                show(frame, emulator: emulator)
            }
        }
        root.position = headPosition
        if menu {
            // A full-screen menu (the map, the pause screens): the whole image
            // on a flat, upright panel where the HUD is, at the HUD's size and
            // with its adjustment.
            let adjustment = hud.adjustment
            screen.orientation = simd_quatf(angle: anchorYaw - adjustment.yaw, axis: [0, 1, 0])
                * simd_quatf(angle: anchorPitch + adjustment.pitch, axis: [1, 0, 0])
            halo.orient(screen.orientation)
            shapeScreen(menu: tangents * WideHud.scale(imageTanY: tangents.y) * adjustment.scale)
        } else if let screenYaw {
            shapeScreen(menu: nil)
            screen.orientation = simd_quatf(angle: screenYaw, axis: [0, 1, 0])
                * simd_quatf(angle: screenPitch, axis: [1, 0, 0])
            halo.orient(screen.orientation)
            // The HUD stays on the game camera's own view: in third person the
            // view the head look started from, in first person the screen.
            if firstPerson {
                hud.setUpright(centredOn: nil)
                hud.orient(screen.orientation)
            } else {
                // Upright, so it stays level however the head turns.
                hud.setUpright(centredOn: hudPitch ?? screenPitch)
                hud.orient(simd_quatf(angle: anchorYaw, axis: [0, 1, 0]))
            }
        }
        log(emulator, headYaw: headYaw, headPitch: headPitch, firstPerson: firstPerson)
    }

    /// Places the frame, in third person, where the head look turned it from
    /// the recentre direction; in first person at its camera's true pitch.
    private func apply(_ frame: DKStereoFrame, headYaw: Float?, headPitch: Float,
                       firstPerson: Bool) {
        let forward = frame.cameraRotation.act([0, 0, -1])
        let cameraYaw = Self.yaw(forward: forward)
        let cameraPitch = asin(max(-1, min(1, forward.y)))
        defer { lastCameraYaw = cameraYaw }
        if frame.hasHeadLook {
            // The view was turned by the head look: show it where the head faced.
            screenYaw = anchorYaw + frame.headLookYaw
            screenPitch = anchorPitch + frame.headLookPitch
            basePitch = cameraPitch - frame.headLookPitch
            // The game camera's own view is at the recentre direction.
            hudPitch = anchorPitch
            firstPersonOffset = nil
            return
        }
        // Third person without a head look (between enabling it and its first
        // frame): keep the placement.
        if !firstPerson && screenYaw != nil {
            return
        }
        // First person: follow the camera at a fixed offset, set in front of
        // the head on entry and after a cut.
        let cut = lastCameraYaw.map { abs(Self.wrap(cameraYaw - $0)) >= cutRadians } ?? true
        if firstPersonOffset == nil || cut {
            firstPersonOffset = Self.wrap((headYaw ?? anchorYaw) - cameraYaw)
        }
        screenYaw = cameraYaw + (firstPersonOffset ?? 0)
        screenPitch = cameraPitch
        hudPitch = nil
        basePitch = nil
    }

    /// Once a second, for tuning on device (user/log/azahar_log.txt).
    private func log(_ emulator: DKEmulator, headYaw: Float?, headPitch: Float,
                     firstPerson: Bool) {
        let now = CACurrentMediaTime()
        guard now - lastLog >= 1, let headYaw else { return }
        lastLog = now
        let degrees = { (radians: Float?) in radians.map { "\(Int(($0 * 180 / .pi).rounded()))" } ?? "-" }
        emulator.logWideView("head \(degrees(Self.wrap(headYaw - anchorYaw))) screen "
            + "\(degrees(screenYaw.map { Self.wrap($0 - anchorYaw) })) camera \(degrees(lastCameraYaw)) "
            + "pitch head \(degrees(headPitch)) anchor \(degrees(anchorPitch)) "
            + "screen \(degrees(screenPitch)) hud \(degrees(hudPitch)) camera \(degrees(basePitch)) "
            + "first_person \(firstPerson) menu \(menu) tangents \(tangents.x) \(tangents.y)")
    }

    /// Builds the screen as a flat panel of `menu`'s angles, or (nil) as the
    /// image's sphere section again.
    private func shapeScreen(menu new: SIMD2<Float>?) {
        guard tangents != .zero, new != menuTangents else { return }
        menuTangents = new
        let shape = new ?? tangents
        updateFeather()
        do {
            // A flat panel's corners stay within `radius`, in front of the
            // halo's sphere, which would otherwise hide all but a circle of it.
            let distance = new == nil ? radius
                : radius / (1 + shape.x * shape.x + shape.y * shape.y).squareRoot()
            screen.model?.mesh = try Self.sphereSection(tangents: shape, radius: distance,
                                                        flat: new != nil)
            halo.resize(tangents: shape, radius: radius)
        } catch {
            NSLog("Dekui: the wide view mesh failed: %@", "\(error)")
        }
    }

    /// Fraction of the image's width that fades into the room at each side.
    private let featherWidth: Float = 0.24

    /// The edge fade covers about the same angle on all four sides.
    private func updateFeather() {
        guard var material, tangents.x > 0, tangents.y > 0,
              material.parameterNames.contains("FeatherU") else { return }
        do {
            // A menu's panel keeps its edges: they are the game screen's.
            let width: Float = menuTangents == nil ? featherWidth : 0.001
            try material.setParameter(name: "FeatherU", value: .float(width))
            try material.setParameter(
                name: "FeatherV", value: .float(min(0.45, width * tangents.x / tangents.y)))
            self.material = material
            screen.model?.materials = [material]
        } catch {
            NSLog("Dekui: the wide view edge fade could not be set: %@", "\(error)")
        }
    }

    /// The stereo screen's material, with its edges fading into the room
    /// (opacity from the mesh's own coordinates, so both eyes fade alike), as
    /// the Photos app's spatial images do.
    static let materialSource: String = {
        let fade = """
                    def Shader "Split"
                    {
                        uniform token info:id = "ND_separate2_vector2"
                        float2 inputs:in.connect = </Root/WideScreen/UV.outputs:out>
                        float outputs:outx
                        float outputs:outy
                    }

                    def Shader "InvU"
                    {
                        uniform token info:id = "ND_subtract_float"
                        float inputs:in1 = 1
                        float inputs:in2.connect = </Root/WideScreen/Split.outputs:outx>
                        float outputs:out
                    }

                    def Shader "InvV"
                    {
                        uniform token info:id = "ND_subtract_float"
                        float inputs:in1 = 1
                        float inputs:in2.connect = </Root/WideScreen/Split.outputs:outy>
                        float outputs:out
                    }

                    def Shader "FadeLeft"
                    {
                        uniform token info:id = "ND_smoothstep_float"
                        float inputs:in.connect = </Root/WideScreen/Split.outputs:outx>
                        float inputs:low = 0
                        float inputs:high.connect = </Root/WideScreen.inputs:FeatherU>
                        float outputs:out
                    }

                    def Shader "FadeRight"
                    {
                        uniform token info:id = "ND_smoothstep_float"
                        float inputs:in.connect = </Root/WideScreen/InvU.outputs:out>
                        float inputs:low = 0
                        float inputs:high.connect = </Root/WideScreen.inputs:FeatherU>
                        float outputs:out
                    }

                    def Shader "FadeBottom"
                    {
                        uniform token info:id = "ND_smoothstep_float"
                        float inputs:in.connect = </Root/WideScreen/Split.outputs:outy>
                        float inputs:low = 0
                        float inputs:high.connect = </Root/WideScreen.inputs:FeatherV>
                        float outputs:out
                    }

                    def Shader "FadeTop"
                    {
                        uniform token info:id = "ND_smoothstep_float"
                        float inputs:in.connect = </Root/WideScreen/InvV.outputs:out>
                        float inputs:low = 0
                        float inputs:high.connect = </Root/WideScreen.inputs:FeatherV>
                        float outputs:out
                    }

                    def Shader "FadeX"
                    {
                        uniform token info:id = "ND_multiply_float"
                        float inputs:in1.connect = </Root/WideScreen/FadeLeft.outputs:out>
                        float inputs:in2.connect = </Root/WideScreen/FadeRight.outputs:out>
                        float outputs:out
                    }

                    def Shader "FadeY"
                    {
                        uniform token info:id = "ND_multiply_float"
                        float inputs:in1.connect = </Root/WideScreen/FadeBottom.outputs:out>
                        float inputs:in2.connect = </Root/WideScreen/FadeTop.outputs:out>
                        float outputs:out
                    }

                    def Shader "Fade"
                    {
                        uniform token info:id = "ND_multiply_float"
                        float inputs:in1.connect = </Root/WideScreen/FadeX.outputs:out>
                        float inputs:in2.connect = </Root/WideScreen/FadeY.outputs:out>
                        float outputs:out
                    }
            """
        var source = StereoScreen.materialSource.replacingOccurrences(
            of: "StereoScreen", with: "WideScreen")
        source = source.replacingOccurrences(
            of: "float inputs:opacity = 1",
            with: "float inputs:opacity.connect = </Root/WideScreen/Fade.outputs:out>")
        source = source.replacingOccurrences(
            of: "float inputs:RightOffset = 0.5\n",
            with: "float inputs:RightOffset = 0.5\n        float inputs:FeatherU = 0.12\n"
                + "        float inputs:FeatherV = 0.12\n")
        // The material's closing brace, then the root's.
        if let end = source.range(of: "\n    }\n}", options: .backwards) {
            source.insert(contentsOf: "\n" + fade, at: end.lowerBound)
        }
        return source
    }()

    /// Rebuilds the sphere section when the frame's angles change, e.g. for a
    /// cutscene camera with its own field of view.
    private func resize(tangents new: SIMD2<Float>) {
        guard new.x > 0.01, new.y > 0.01, new.x < 20, new.y < 20 else { return }
        if tangents != .zero, abs(new.x / tangents.x - 1) < 0.005,
           abs(new.y / tangents.y - 1) < 0.005 {
            return
        }
        tangents = new
        menuTangents = nil
        updateFeather()
        do {
            screen.model?.mesh = try Self.sphereSection(tangents: new, radius: radius)
            halo.resize(tangents: new, radius: radius)
            hud.resize(imageTangents: new)
        } catch {
            NSLog("Dekui: the wide view mesh failed: %@", "\(error)")
        }
    }

    private func show(_ frame: DKStereoFrame, emulator: DKEmulator) {
        guard var material else {
            droppedFrames += 1
            if droppedFrames % 60 == 1 {
                emulator.logWideView("\(droppedFrames) frames dropped: no material")
            }
            emulator.releaseStereoFrame(frame)
            return
        }
        let source = frame.texture
        let prepared = upscaler.prepare(source: source, eyes: frame.eyes,
                                        mode: upscaling, antialiasing: antialiasing)
        let outputWidth = prepared?.width ?? source.width
        let outputHeight = prepared?.height ?? source.height
        let outputFormat = prepared?.pixelFormat ?? source.pixelFormat
        do {
            var changed = false
            if texture?.descriptor.width != outputWidth
                || texture?.descriptor.height != outputHeight
                || texture?.descriptor.pixelFormat != outputFormat {
                let descriptor = LowLevelTexture.Descriptor(
                    pixelFormat: outputFormat, width: outputWidth,
                    height: outputHeight, textureUsage: [.shaderRead])
                let texture = try LowLevelTexture(descriptor: descriptor)
                try material.setParameter(
                    name: "Frame", value: .textureResource(TextureResource(from: texture)))
                self.texture = texture
                changed = true
            }
            if frame.eyes != eyes {
                eyes = frame.eyes
                let stereo = eyes == 2
                try material.setParameter(name: "EyeScale", value: .float(stereo ? 0.5 : 1))
                try material.setParameter(name: "RightOffset", value: .float(stereo ? 0.5 : 0))
                changed = true
            }
            if changed {
                self.material = material
                screen.model?.materials = [material]
            }
        } catch {
            NSLog("Dekui: the wide view texture could not be updated: %@", "\(error)")
            emulator.logWideView("texture update failed: \(error) "
                + "\(outputWidth)x\(outputHeight) \(outputFormat.rawValue)")
            emulator.releaseStereoFrame(frame)
            return
        }
        if queue == nil {
            queue = source.device.makeCommandQueue()
        }
        guard let texture, let command = queue?.makeCommandBuffer() else {
            emulator.releaseStereoFrame(frame)
            return
        }
        let destination = texture.replace(using: command)
        if prepared != nil {
            guard upscaler.encode(source: source, eyes: frame.eyes, destination: destination,
                                  antialiasing: antialiasing, command: command) else {
                emulator.releaseStereoFrame(frame)
                return
            }
        } else if let blit = command.makeBlitCommandEncoder() {
            blit.copy(from: source, to: destination)
            blit.endEncoding()
        }
        // A menu's frame holds the whole screen; the HUD plane would cover it,
        // and the glow would spread its contents (a cutscene's) around it.
        if frame.isMenu {
            hud.hide()
            halo.hide()
        } else {
            halo.encode(from: source, eyes: frame.eyes, command: command)
            hud.encode(from: frame.hudTexture, host: frame.hostHudTexture,
                       eyes: frame.eyes, command: command)
        }
        command.addCompletedHandler { _ in emulator.releaseStereoFrame(frame) }
        command.commit()
        screen.isEnabled = tangents != .zero
    }

    /// A grid over the frame's image plane, pushed out onto a sphere. Every
    /// vertex lies on the ray its pixel was rendered along, so the image is
    /// undistorted from the centre.
    static func sphereSection(
        tangents: SIMD2<Float>, radius: Float, flat: Bool = false,
        uv: (Float, Float) -> SIMD2<Float> = { u, v in [u, 1 - v] }
    ) throws -> MeshResource {
        // Dense enough that the chord between adjacent vertices stays behind
        // the near edge even at the 120-degree setting. The coarser grid could
        // show a centre seam/clip as the surface wrapped far into the periphery.
        let columns = 96, rows = 64
        var positions: [SIMD3<Float>] = []
        var uvs: [SIMD2<Float>] = []
        positions.reserveCapacity((columns + 1) * (rows + 1))
        for row in 0...rows {
            let v = Float(row) / Float(rows)
            for column in 0...columns {
                let u = Float(column) / Float(columns)
                let ray = SIMD3((2 * u - 1) * tangents.x, (1 - 2 * v) * tangents.y, -1)
                // Flat: the image plane itself, `radius` away.
                positions.append(flat ? ray * radius : normalize(ray) * radius)
                // `u` runs left to right and `v` top to bottom across the
                // section; RealityKit's texture V runs bottom to top.
                uvs.append(uv(u, v))
            }
        }
        var indices: [UInt32] = []
        for row in 0..<rows {
            for column in 0..<columns {
                let topLeft = UInt32(row * (columns + 1) + column)
                let bottomLeft = topLeft + UInt32(columns + 1)
                indices += [topLeft, bottomLeft, topLeft + 1,
                            bottomLeft, bottomLeft + 1, topLeft + 1]
            }
        }
        var descriptor = MeshDescriptor(name: "WideView")
        descriptor.positions = MeshBuffers.Positions(positions)
        descriptor.textureCoordinates = MeshBuffers.TextureCoordinates(uvs)
        descriptor.primitives = .triangles(indices)
        return try MeshResource.generate(from: [descriptor])
    }

    /// Yaw of a forward vector about +y, 0 looking down -z.
    static func yaw(forward: SIMD3<Float>) -> Float {
        atan2(-forward.x, -forward.z)
    }

    static func wrap(_ angle: Float) -> Float {
        var a = angle
        while a > .pi { a -= 2 * .pi }
        while a < -.pi { a += 2 * .pi }
        return a
    }
}
