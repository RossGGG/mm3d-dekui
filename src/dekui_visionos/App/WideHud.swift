// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Metal
import RealityKit

/// The immersive wide view's HUD on its own plane. The emulator draws the HUD
/// into a separate, supersampled layer instead of the game image, and this
/// shows it slightly in front of the screen, where the game camera's own
/// view is in the room (on the character), so it neither follows the head
/// nor moves with the game's frames.
@MainActor
final class WideHud {
    /// Shared world transform for both independently visible HUD layers.
    let anchor = Entity()
    let entity = ModelEntity(mesh: .generatePlane(width: 1, height: 1))

    /// Metres from the head; the game image is further away.
    private let radius: Float = 3.4

    private var material: ShaderGraphMaterial?
    private var texture: LowLevelTexture?
    private var compositePipeline: MTLComputePipelineState?
    private var tangents = SIMD2<Float>.zero
    /// The elevation the upright surface is centred on (third person).
    private var centrePitch: Float?
    /// Whether the mesh is the screen-aligned kind (first person).
    private var screenAligned = true

    /// The player's own adjustment on top of the automatic placement: turned
    /// right and raised (radians) and scaled, from dragging and pinching the
    /// HUD with both hands. Saved across launches.
    struct Adjustment: Equatable {
        var yaw: Float = 0
        var pitch: Float = 0
        var scale: Float = 1

        static let maxYaw: Float = 1.0
        static let maxPitch: Float = 0.8
        static let scaleRange: ClosedRange<Float> = 0.4...2.5

        func clamped() -> Adjustment {
            Adjustment(yaw: max(-Self.maxYaw, min(Self.maxYaw, yaw)),
                       pitch: max(-Self.maxPitch, min(Self.maxPitch, pitch)),
                       scale: max(Self.scaleRange.lowerBound, min(Self.scaleRange.upperBound, scale)))
        }

        static func load() -> Adjustment {
            let defaults = UserDefaults.standard
            return Adjustment(yaw: defaults.float(forKey: "DekuiHudYaw"),
                              pitch: defaults.float(forKey: "DekuiHudPitch"),
                              scale: defaults.object(forKey: "DekuiHudScale") as? Float ?? 1).clamped()
        }

        func save() {
            let defaults = UserDefaults.standard
            defaults.set(yaw, forKey: "DekuiHudYaw")
            defaults.set(pitch, forKey: "DekuiHudPitch")
            defaults.set(scale, forKey: "DekuiHudScale")
        }
    }

    var adjustment = Adjustment.load() {
        didSet {
            adjustment = adjustment.clamped()
            if adjustment.scale != oldValue.scale || adjustment.pitch != oldValue.pitch {
                rebuild()
            }
            orient(baseOrientation)
        }
    }
    private var baseOrientation = simd_quatf(angle: 0, axis: [0, 1, 0])

    /// HUD adjustment mode: only then is the HUD a target for pinches, so
    /// otherwise they never reach it instead of the game's controls.
    var adjustable = false {
        didSet {
            guard adjustable != oldValue else { return }
            updateTarget()
        }
    }
    /// A coarse hull of the surface, enough to aim a pinch at (rebuild).
    private var targetShape: ShapeResource?

    init() {
        entity.isEnabled = false
        // Native and host UI are composited into this one surface.
        anchor.addChild(entity)
    }

    /// Pinch and drag to move it, pinch with both hands to scale it
    /// (DimSurroundings), while `adjustable`.
    private func updateTarget() {
        if adjustable, let targetShape {
            entity.components.set(InputTargetComponent())
            entity.components.set(CollisionComponent(shapes: [targetShape]))
        } else {
            entity.components.remove(InputTargetComponent.self)
            entity.components.remove(CollisionComponent.self)
        }
    }

    /// Turns the HUD to the automatic placement's orientation plus the
    /// player's turn (and, screen-aligned, their raise; upright, the raise is
    /// in the mesh so it stays level).
    func orient(_ base: simd_quatf) {
        baseOrientation = base
        var orientation = base * simd_quatf(angle: -adjustment.yaw, axis: [0, 1, 0])
        if screenAligned {
            orientation *= simd_quatf(angle: adjustment.pitch, axis: [1, 0, 0])
        }
        anchor.orientation = orientation
    }

    /// The direction of a point in the HUD's parent's space, from the head:
    /// rightward yaw and upward pitch, for dragging.
    static func angles(of point: SIMD3<Float>) -> SIMD2<Float> {
        let length = max(0.001, simd_length(point))
        return [atan2(point.x, -point.z), asin(max(-1, min(1, point.y / length)))]
    }

    func loadMaterial() async {
        guard material == nil else { return }
        do {
            var loaded = try await ShaderGraphMaterial(
                named: "/Root/Hud", from: Data(Self.materialSource.utf8))
            loaded.faceCulling = .none
            material = loaded
        } catch {
            NSLog("Dekui: the wide view HUD material failed to load: %@", "\(error)")
            DKEmulator.shared.logWideView("HUD material failed: \(error)")
        }
    }

    func hide() {
        entity.isEnabled = false
    }

    /// The part of the game image's angles the HUD covers: about the angle
    /// it covers on a 3DS, times `spread` (as MM3D::WideView::HudScale).
    static func scale(imageTanY: Float) -> Float {
        let nativeTanY: Float = 0.41421356 // tan(22.5 degrees)
        let spread: Float = 1.2
        return imageTanY > nativeTanY ? min(1, spread * nativeTanY / imageTanY) : 1
    }

    /// The game image's angles; the HUD covers a 3DS-sized part of them.
    func resize(imageTangents: SIMD2<Float>) {
        let new = imageTangents * Self.scale(imageTanY: imageTangents.y)
        guard new.x > 0.01, new.y > 0.01 else { return }
        if tangents != .zero, abs(new.x / tangents.x - 1) < 0.005,
           abs(new.y / tangents.y - 1) < 0.005 {
            return
        }
        tangents = new
        rebuild()
    }

    /// Third person: an upright surface (rows along lines of latitude) centred
    /// on `pitch` and on the entity's heading, so the HUD stays level wherever
    /// the head turns. Nil: aligned with the screen (first person), turned by
    /// the entity's orientation.
    func setUpright(centredOn pitch: Float?) {
        if let pitch {
            if !screenAligned, let current = centrePitch, abs(current - pitch) < 0.008 {
                return
            }
            let wasAligned = screenAligned
            centrePitch = pitch
            screenAligned = false
            if wasAligned { orient(baseOrientation) }
        } else {
            if screenAligned { return }
            screenAligned = true
            centrePitch = nil
            orient(baseOrientation)
        }
        rebuild()
    }

    private func rebuild() {
        guard tangents != .zero else { return }
        let tangents = self.tangents * adjustment.scale
        // The HUD texture is the guest framebuffer, rotated: its x runs up the
        // screen from the bottom, its y leftward from the right. With
        // RealityKit's V (bottom to top), display (u, v) samples (1 - v, u).
        let uv: (Float, Float) -> SIMD2<Float> = { u, v in [1 - v, u] }
        do {
            let point: (Float, Float) -> SIMD3<Float>
            if let pitch = centrePitch, !screenAligned {
                let halfAngles = SIMD2(atan(tangents.x), atan(tangents.y))
                let centre = pitch + adjustment.pitch
                entity.model?.mesh = try Self.uprightSection(
                    halfAngles: halfAngles, centrePitch: centre, radius: radius, uv: uv)
                point = { u, v in
                    let elevation = centre + (1 - 2 * v) * halfAngles.y
                    let azimuth = (2 * u - 1) * halfAngles.x
                    return SIMD3(sin(azimuth) * cos(elevation), sin(elevation),
                                 -cos(azimuth) * cos(elevation)) * self.radius
                }
            } else {
                entity.model?.mesh = try WideScreen.sphereSection(
                    tangents: tangents, radius: radius, uv: uv)
                point = { u, v in
                    normalize(SIMD3((2 * u - 1) * tangents.x, (1 - 2 * v) * tangents.y, -1))
                        * self.radius
                }
            }
            var hull: [SIMD3<Float>] = []
            for row in 0...6 {
                for column in 0...8 {
                    hull.append(point(Float(column) / 8, Float(row) / 6))
                }
            }
            targetShape = ShapeResource.generateConvex(from: hull)
            updateTarget()
        } catch {
            NSLog("Dekui: the wide view HUD mesh failed: %@", "\(error)")
        }
    }

    /// A section of the sphere between two meridians and two parallels,
    /// looking down -z, centred on `centrePitch` (up positive).
    static func uprightSection(halfAngles: SIMD2<Float>, centrePitch: Float, radius: Float,
                               uv: (Float, Float) -> SIMD2<Float>) throws -> MeshResource {
        let columns = 32, rows = 24
        var positions: [SIMD3<Float>] = []
        var uvs: [SIMD2<Float>] = []
        for row in 0...rows {
            let v = Float(row) / Float(rows)
            let elevation = centrePitch + (1 - 2 * v) * halfAngles.y
            for column in 0...columns {
                let u = Float(column) / Float(columns)
                let azimuth = (2 * u - 1) * halfAngles.x // right positive
                positions.append(SIMD3(sin(azimuth) * cos(elevation), sin(elevation),
                                       -cos(azimuth) * cos(elevation)) * radius)
                uvs.append(uv(u, v))
            }
        }
        var indices: [UInt32] = []
        for row in 0..<rows {
            for column in 0..<columns {
                let topLeft = UInt32(row * (columns + 1) + column)
                let bottomLeft = topLeft + UInt32(columns + 1)
                indices += [topLeft, bottomLeft, topLeft + 1, bottomLeft, bottomLeft + 1, topLeft + 1]
            }
        }
        var descriptor = MeshDescriptor(name: "WideHud")
        descriptor.positions = MeshBuffers.Positions(positions)
        descriptor.textureCoordinates = MeshBuffers.TextureCoordinates(uvs)
        descriptor.primitives = .triangles(indices)
        return try MeshResource.generate(from: [descriptor])
    }

    /// Composite both HUD producers before presentation. The host texture is
    /// display-oriented (and stereo); the native texture is rotated. Converting
    /// at this boundary gives every element the same mesh, UVs and world pose.
    func encode(from hud: MTLTexture?, host: MTLTexture?, eyes: Int,
                command: MTLCommandBuffer) {
        guard let source = hud ?? host, var material else {
            entity.isEnabled = false
            return
        }
        let width = hud?.width ?? source.height
        let height = hud?.height ?? (source.width / max(1, eyes))
        do {
            if compositePipeline == nil {
                let library = try source.device.makeLibrary(source: Self.compositeSource, options: nil)
                guard let function = library.makeFunction(name: "composite_hud") else { return }
                compositePipeline = try source.device.makeComputePipelineState(function: function)
            }
            if texture?.descriptor.width != width || texture?.descriptor.height != height {
                let descriptor = LowLevelTexture.Descriptor(
                    pixelFormat: .rgba16Float, width: width, height: height,
                    textureUsage: [.shaderRead, .shaderWrite])
                let texture = try LowLevelTexture(descriptor: descriptor)
                try material.setParameter(
                    name: "Frame", value: .textureResource(TextureResource(from: texture)))
                self.texture = texture
                self.material = material
                entity.model?.materials = [material]
            }
        } catch {
            DKEmulator.shared.logWideView("HUD composite failed: \(error)")
            return
        }
        guard let texture, let compositePipeline else { return }
        let destination = texture.replace(using: command)
        guard let encoder = command.makeComputeCommandEncoder() else { return }
        encoder.label = "Dekui unified immersive HUD"
        encoder.setComputePipelineState(compositePipeline)
        encoder.setTexture(hud ?? source, index: 0)
        encoder.setTexture(host ?? source, index: 1)
        encoder.setTexture(destination, index: 2)
        var flags = SIMD4<UInt32>(hud == nil ? 0 : 1, host == nil ? 0 : 1,
                                 UInt32(max(1, eyes)), 0)
        encoder.setBytes(&flags, length: MemoryLayout<SIMD4<UInt32>>.stride, index: 0)
        encoder.dispatchThreads(MTLSize(width: width, height: height, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        encoder.endEncoding()
        entity.isEnabled = tangents != .zero
    }

    static let compositeSource = """
        #include <metal_stdlib>
        using namespace metal;
        kernel void composite_hud(texture2d<float, access::sample> native [[texture(0)]],
                                  texture2d<float, access::sample> host [[texture(1)]],
                                  texture2d<half, access::write> output [[texture(2)]],
                                  constant uint4& flags [[buffer(0)]],
                                  uint2 pixel [[thread_position_in_grid]]) {
            if (pixel.x >= output.get_width() || pixel.y >= output.get_height()) return;
            constexpr sampler sampleHUD(coord::normalized, address::clamp_to_edge, filter::linear);
            float2 uv = (float2(pixel) + 0.5) / float2(output.get_width(), output.get_height());
            float4 guest = flags.x ? native.sample(sampleHUD, uv) : float4(0);
            // Guest texture Y runs leftward from the display's right edge;
            // host texture X runs rightward. Reverse that axis before taking
            // the left stereo half (both halves contain identical host UI).
            float2 hostUV((1.0 - uv.y) / float(flags.z), 1.0 - uv.x);
            float4 overlay = flags.y ? host.sample(sampleHUD, hostUV) : float4(0);
            output.write(half4(overlay + guest * (1.0 - overlay.a)), pixel);
        }
        """

    /// Unlit, premultiplied colour with the layer's coverage as opacity.
    static let materialSource = """
        #usda 1.0
        (
            defaultPrim = "Root"
            metersPerUnit = 1
            upAxis = "Y"
        )

        def Xform "Root"
        {
            def Material "Hud"
            {
                asset inputs:Frame
                token outputs:mtlx:surface.connect = </Root/Hud/Surface.outputs:out>
                token outputs:realitykit:vertex

                def Shader "Surface"
                {
                    uniform token info:id = "ND_realitykit_unlit_surfaceshader"
                    bool inputs:applyPostProcessToneMap = 0
                    color3f inputs:color.connect = </Root/Hud/RGB.outputs:out>
                    bool inputs:hasPremultipliedAlpha = 1
                    float inputs:opacity.connect = </Root/Hud/Channels.outputs:outa>
                    token outputs:out
                }

                def Shader "UV"
                {
                    uniform token info:id = "ND_texcoord_vector2"
                    float2 outputs:out
                }

                def Shader "Image"
                {
                    uniform token info:id = "ND_RealityKitTexture2D_color4"
                    asset inputs:file.connect = </Root/Hud.inputs:Frame>
                    float2 inputs:texcoord.connect = </Root/Hud/UV.outputs:out>
                    bool inputs:no_flip_v = 1
                    color4f outputs:out
                }

                def Shader "RGB"
                {
                    uniform token info:id = "ND_convert_color4_color3"
                    color4f inputs:in.connect = </Root/Hud/Image.outputs:out>
                    color3f outputs:out
                }

                def Shader "Channels"
                {
                    uniform token info:id = "ND_separate4_color4"
                    color4f inputs:in.connect = </Root/Hud/Image.outputs:out>
                    float outputs:outa
                    float outputs:outb
                    float outputs:outg
                    float outputs:outr
                }
            }
        }
        """

}
