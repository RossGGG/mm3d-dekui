// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Metal
import RealityKit
import SwiftUI

/// Shows the game in stereo 3D. The renderer draws both eyes side by side; a
/// RealityKit plane covering the game view samples the left half for the left
/// eye and the right half for the right eye, which a CAMetalLayer cannot do.
@MainActor
final class StereoScreen {
    let root = Entity()
    private let screen = ModelEntity(mesh: .generatePlane(width: 1, height: 1))
    private var material: ShaderGraphMaterial?
    private var texture: LowLevelTexture?
    private var queue: MTLCommandQueue?
    private var eyes = 0

    init() {
        screen.isEnabled = false
        root.addChild(screen)
    }

    func loadMaterial() async {
        guard material == nil else { return }
        do {
            material = try await ShaderGraphMaterial(
                named: "/Root/StereoScreen", from: Data(Self.materialSource.utf8))
        } catch {
            NSLog("Dekui: the stereo material failed to load: %@", "\(error)")
        }
    }

    private var size: SIMD2<Float> = .zero

    /// Covers `bounds`, the game view's frame in `root`'s space, on
    /// its back face: the window's surface. RealityKit's origin in a window is
    /// not on that surface, so the frame is converted rather than assumed.
    /// The plane is rebuilt rather than scaled so its rounded corners stay round.
    func place(in bounds: BoundingBox, cornerRadius: Float) {
        let width = bounds.extents.x, height = bounds.extents.y
        guard width > 0, height > 0 else { return }
        // A fraction of a millimeter in front of the window's own layer.
        screen.position = [bounds.center.x, bounds.center.y, bounds.min.z + 0.0002]
        guard SIMD2(width, height) != size else { return }
        size = SIMD2(width, height)
        let radius = min(cornerRadius, min(width, height) / 2)
        screen.model?.mesh = MeshResource.generatePlane(width: width, height: height,
                                                        cornerRadius: radius)
    }

    /// Hidden until the next frame, so stale frames never show.
    func hide() {
        screen.isEnabled = false
    }

    /// Copies the newest frame into the plane's texture. Call once per display frame.
    func update(from emulator: DKEmulator) {
        guard var material, let frame = emulator.acquireStereoFrame() else { return }
        let source = frame.texture
        do {
            var changed = false
            if texture?.descriptor.width != source.width
                || texture?.descriptor.height != source.height {
                let descriptor = LowLevelTexture.Descriptor(
                    pixelFormat: source.pixelFormat, width: source.width,
                    height: source.height, textureUsage: [.shaderRead])
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
            NSLog("Dekui: the stereo texture could not be updated: %@", "\(error)")
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
        if let blit = command.makeBlitCommandEncoder() {
            blit.copy(from: source, to: destination)
            blit.endEncoding()
        }
        command.addCompletedHandler { _ in emulator.releaseStereoFrame(frame) }
        command.commit()
        screen.isEnabled = true
    }

    /// An unlit material whose texture coordinates depend on the eye being
    /// rendered (RealityKit's camera index switch). EyeScale and RightOffset
    /// select the halves of a side-by-side frame; 1 and 0 show the whole frame.
    /// Mono views (screenshots, the simulator) see the left eye. The plane's
    /// texture coordinates already put row 0 at the top, so V is not flipped.
    static let materialSource = """
        #usda 1.0
        (
            defaultPrim = "Root"
            metersPerUnit = 1
            upAxis = "Y"
        )

        def Xform "Root"
        {
            def Material "StereoScreen"
            {
                asset inputs:Frame
                float inputs:EyeScale = 0.5
                float inputs:RightOffset = 0.5
                token outputs:mtlx:surface.connect = </Root/StereoScreen/Surface.outputs:out>
                token outputs:realitykit:vertex

                def Shader "Surface"
                {
                    uniform token info:id = "ND_realitykit_unlit_surfaceshader"
                    bool inputs:applyPostProcessToneMap = 0
                    color3f inputs:color.connect = </Root/StereoScreen/Image.outputs:out>
                    bool inputs:hasPremultipliedAlpha = 0
                    float inputs:opacity = 1
                    token outputs:out
                }

                def Shader "Image"
                {
                    uniform token info:id = "ND_RealityKitTexture2D_color3"
                    asset inputs:file.connect = </Root/StereoScreen.inputs:Frame>
                    float2 inputs:texcoord.connect = </Root/StereoScreen/EyeUV.outputs:out>
                    bool inputs:no_flip_v = 1
                    color3f outputs:out
                }

                def Shader "UV"
                {
                    uniform token info:id = "ND_texcoord_vector2"
                    float2 outputs:out
                }

                def Shader "EyeScale2"
                {
                    uniform token info:id = "ND_combine2_vector2"
                    float inputs:in1.connect = </Root/StereoScreen.inputs:EyeScale>
                    float inputs:in2 = 1
                    float2 outputs:out
                }

                def Shader "LeftUV"
                {
                    uniform token info:id = "ND_multiply_vector2"
                    float2 inputs:in1.connect = </Root/StereoScreen/UV.outputs:out>
                    float2 inputs:in2.connect = </Root/StereoScreen/EyeScale2.outputs:out>
                    float2 outputs:out
                }

                def Shader "RightShift"
                {
                    uniform token info:id = "ND_combine2_vector2"
                    float inputs:in1.connect = </Root/StereoScreen.inputs:RightOffset>
                    float inputs:in2 = 0
                    float2 outputs:out
                }

                def Shader "RightUV"
                {
                    uniform token info:id = "ND_add_vector2"
                    float2 inputs:in1.connect = </Root/StereoScreen/LeftUV.outputs:out>
                    float2 inputs:in2.connect = </Root/StereoScreen/RightShift.outputs:out>
                    float2 outputs:out
                }

                def Shader "EyeUV"
                {
                    uniform token info:id = "ND_realitykit_geometry_switch_cameraindex_vector2"
                    float2 inputs:mono.connect = </Root/StereoScreen/LeftUV.outputs:out>
                    float2 inputs:left.connect = </Root/StereoScreen/LeftUV.outputs:out>
                    float2 inputs:right.connect = </Root/StereoScreen/RightUV.outputs:out>
                    float2 outputs:out
                }
            }
        }
        """
}

/// The stereo presentation over the game view. It ignores touches, which go
/// to the game view underneath with the same coordinates.
struct StereoGameView: View {
    /// visionOS window corners, in points, so the plane matches the 2D view.
    static let windowCornerRadius: CGFloat = 46

    let screen: StereoScreen
    @Environment(\.physicalMetrics) private var metrics

    var body: some View {
        GeometryReader3D { proxy in
            RealityView { content in
                await screen.loadMaterial()
                content.add(screen.root)
            } update: { content in
                screen.place(
                    in: content.convert(proxy.frame(in: .local), from: .local, to: screen.root),
                    cornerRadius: Float(metrics.convert(Self.windowCornerRadius, to: .meters)))
            }
        }
        .allowsHitTesting(false)
    }
}
