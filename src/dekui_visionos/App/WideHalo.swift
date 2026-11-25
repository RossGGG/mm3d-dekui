// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Metal
import MetalPerformanceShaders
import RealityKit
import os

/// The immersive wide view's out-of-focus border, after the Photos app's
/// spatial images: the game image's colours spread outward as a soft glow
/// and fade into the room, so the image has no hard edge to find.
///
/// Each frame, the image (the left eye in 3D) is shrunk into the middle of a
/// small transparent canvas and blurred; the blur carries its colours out
/// over the transparent margin. The canvas covers `scale` times the image's
/// angles on a sphere section just behind the screen, whose own feathered
/// edge blends into it.
@MainActor
final class WideHalo {
    /// The glow just behind the screen, and a larger one further away, so
    /// the glow has depth.
    let entity = ModelEntity(mesh: .generatePlane(width: 1, height: 1))
    let farEntity = ModelEntity(mesh: .generatePlane(width: 1, height: 1))
    /// The far layer's distance and extent relative to the near one.
    private let farDistance: Float = 1.5
    private let farSpread: Float = 1.15

    /// The halo's extent, as a multiple of the image's (in tangent space).
    let scale: Float = 3.2
    /// Canvas size; the image fills the middle 1/scale of it.
    private let canvasWidth = 320
    private let canvasHeight = 192
    /// Blur radius in canvas pixels.
    private let sigma: Float = 30

    private var material: ShaderGraphMaterial?
    private var texture: LowLevelTexture?
    private var canvas: MTLTexture?
    private var scaler: MPSImageBilinearScale?
    private var blur: MPSImageGaussianBlur?
    /// The image's average colour (linear RGB), for tinting the room: the
    /// canvas mipmapped down to one pixel, read back after the GPU finishes.
    private var mipmapped: MTLTexture?
    private var readback: MTLBuffer?
    private let average = OSAllocatedUnfairLock<SIMD3<Float>?>(initialState: nil)
    var averageColor: SIMD3<Float>? { average.withLock { $0 } }

    init() {
        entity.isEnabled = false
        farEntity.isEnabled = false
    }

    func orient(_ orientation: simd_quatf) {
        entity.orientation = orientation
        farEntity.orientation = orientation
    }

    func loadMaterial() async {
        guard material == nil else { return }
        do {
            var loaded = try await ShaderGraphMaterial(
                named: "/Root/Halo", from: Data(Self.materialSource.utf8))
            loaded.faceCulling = .none
            material = loaded
        } catch {
            NSLog("Dekui: the wide view halo material failed to load: %@", "\(error)")
            DKEmulator.shared.logWideView("halo material failed: \(error)")
        }
    }

    /// The image's angles changed: covers `scale` times them, just behind
    /// the screen at `radius`.
    func resize(tangents: SIMD2<Float>, radius: Float) {
        do {
            entity.model?.mesh = try WideScreen.sphereSection(
                tangents: tangents * scale, radius: radius * 1.02)
            farEntity.model?.mesh = try WideScreen.sphereSection(
                tangents: tangents * scale * farSpread, radius: radius * 1.02 * farDistance)
        } catch {
            NSLog("Dekui: the wide view halo mesh failed: %@", "\(error)")
        }
    }

    func hide() {
        entity.isEnabled = false
        farEntity.isEnabled = false
    }

    /// Encodes the blurred canvas for `source` (a frame, both eyes side by
    /// side when `eyes` is 2) into `command`.
    func encode(from source: MTLTexture, eyes: Int, command: MTLCommandBuffer) {
        guard var material else { return }
        let device = source.device
        do {
            if texture == nil {
                let descriptor = LowLevelTexture.Descriptor(
                    pixelFormat: .rgba16Float, width: canvasWidth, height: canvasHeight,
                    textureUsage: [.shaderRead, .shaderWrite])
                let texture = try LowLevelTexture(descriptor: descriptor)
                try material.setParameter(
                    name: "Frame", value: .textureResource(TextureResource(from: texture)))
                self.texture = texture
                self.material = material
                entity.model?.materials = [material]
                farEntity.model?.materials = [material]
            }
        } catch {
            NSLog("Dekui: the wide view halo texture failed: %@", "\(error)")
            return
        }
        if canvas == nil {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba16Float, width: canvasWidth, height: canvasHeight,
                mipmapped: false)
            descriptor.usage = [.shaderRead, .shaderWrite, .renderTarget]
            descriptor.storageMode = .private
            canvas = device.makeTexture(descriptor: descriptor)
            scaler = MPSImageBilinearScale(device: device)
            blur = MPSImageGaussianBlur(device: device, sigma: sigma)
            blur?.edgeMode = .zero
        }
        guard let texture, let canvas, let scaler, let blur else { return }

        // A transparent canvas...
        let clear = MTLRenderPassDescriptor()
        clear.colorAttachments[0].texture = canvas
        clear.colorAttachments[0].loadAction = .clear
        clear.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        clear.colorAttachments[0].storeAction = .store
        command.makeRenderCommandEncoder(descriptor: clear)?.endEncoding()

        // ...with the image (the left eye) shrunk into its middle...
        let imageWidth = Double(canvasWidth) / Double(scale)
        let imageHeight = Double(canvasHeight) / Double(scale)
        let x0 = (Double(canvasWidth) - imageWidth) / 2
        let y0 = (Double(canvasHeight) - imageHeight) / 2
        let eyeWidth = Double(source.width) / Double(max(1, eyes))
        var transform = MPSScaleTransform(
            scaleX: imageWidth / eyeWidth, scaleY: imageHeight / Double(source.height),
            translateX: x0, translateY: y0)
        scaler.clipRect = MTLRegionMake2D(Int(x0.rounded(.down)), Int(y0.rounded(.down)),
                                          Int(imageWidth.rounded(.up)), Int(imageHeight.rounded(.up)))
        withUnsafePointer(to: &transform) { pointer in
            scaler.scaleTransform = pointer
            scaler.encode(commandBuffer: command, sourceTexture: source, destinationTexture: canvas)
        }
        scaler.scaleTransform = nil

        // ...blurred out over the margin, into the halo's texture.
        let destination = texture.replace(using: command)
        blur.encode(commandBuffer: command, sourceTexture: canvas, destinationTexture: destination)
        encodeAverage(from: canvas, command: command)
        entity.isEnabled = true
        farEntity.isEnabled = true
    }

    /// Averages the canvas (premultiplied, the image in its middle) into one
    /// pixel and reads it back once `command` completes.
    private func encodeAverage(from canvas: MTLTexture, command: MTLCommandBuffer) {
        let device = canvas.device
        if mipmapped == nil {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: canvas.pixelFormat, width: canvas.width, height: canvas.height,
                mipmapped: true)
            descriptor.usage = [.shaderRead]
            descriptor.storageMode = .private
            mipmapped = device.makeTexture(descriptor: descriptor)
            readback = device.makeBuffer(length: 8, options: .storageModeShared)
        }
        guard let mipmapped, let readback, let blit = command.makeBlitCommandEncoder() else {
            return
        }
        blit.copy(from: canvas, sourceSlice: 0, sourceLevel: 0, to: mipmapped,
                  destinationSlice: 0, destinationLevel: 0, sliceCount: 1, levelCount: 1)
        blit.generateMipmaps(for: mipmapped)
        let last = mipmapped.mipmapLevelCount - 1
        blit.copy(from: mipmapped, sourceSlice: 0, sourceLevel: last,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: 1, height: 1, depth: 1),
                  to: readback, destinationOffset: 0, destinationBytesPerRow: 8,
                  destinationBytesPerImage: 8)
        blit.endEncoding()
        let average = average
        command.addCompletedHandler { _ in
            let half = readback.contents().assumingMemoryBound(to: Float16.self)
            let alpha = Float(half[3])
            guard alpha > 0.001 else { return }
            let colour = SIMD3(Float(half[0]), Float(half[1]), Float(half[2])) / alpha
            average.withLock { $0 = colour }
        }
    }

    /// Unlit: the blurred canvas's colour, unpremultiplied and brightened so the
/// fringe keeps the image's colour rather than greying out, with its alpha
/// boosted.
    static let materialSource = """
        #usda 1.0
        (
            defaultPrim = "Root"
            metersPerUnit = 1
            upAxis = "Y"
        )

        def Xform "Root"
        {
            def Material "Halo"
            {
                asset inputs:Frame
                token outputs:mtlx:surface.connect = </Root/Halo/Surface.outputs:out>
                token outputs:realitykit:vertex

                def Shader "Surface"
                {
                    uniform token info:id = "ND_realitykit_unlit_surfaceshader"
                    bool inputs:applyPostProcessToneMap = 0
                    color3f inputs:color.connect = </Root/Halo/Bright.outputs:out>
                    bool inputs:hasPremultipliedAlpha = 0
                    float inputs:opacity.connect = </Root/Halo/Opacity.outputs:out>
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
                    asset inputs:file.connect = </Root/Halo.inputs:Frame>
                    float2 inputs:texcoord.connect = </Root/Halo/UV.outputs:out>
                    bool inputs:no_flip_v = 1
                    color4f outputs:out
                }

                def Shader "RGB"
                {
                    uniform token info:id = "ND_convert_color4_color3"
                    color4f inputs:in.connect = </Root/Halo/Image.outputs:out>
                    color3f outputs:out
                }

                def Shader "Coverage"
                {
                    uniform token info:id = "ND_max_float"
                    float inputs:in1.connect = </Root/Halo/Channels.outputs:outa>
                    float inputs:in2 = 0.001
                    float outputs:out
                }

                def Shader "Unpremultiplied"
                {
                    uniform token info:id = "ND_divide_color3FA"
                    color3f inputs:in1.connect = </Root/Halo/RGB.outputs:out>
                    float inputs:in2.connect = </Root/Halo/Coverage.outputs:out>
                    color3f outputs:out
                }

                def Shader "Bright"
                {
                    uniform token info:id = "ND_multiply_color3FA"
                    color3f inputs:in1.connect = </Root/Halo/Unpremultiplied.outputs:out>
                    float inputs:in2 = 1.18
                    color3f outputs:out
                }

                def Shader "Boost"
                {
                    uniform token info:id = "ND_multiply_float"
                    float inputs:in1.connect = </Root/Halo/Channels.outputs:outa>
                    float inputs:in2 = 2.8
                    float outputs:out
                }

                def Shader "Opacity"
                {
                    uniform token info:id = "ND_clamp_float"
                    float inputs:in.connect = </Root/Halo/Boost.outputs:out>
                    float inputs:low = 0
                    float inputs:high = 0.82
                    float outputs:out
                }

                def Shader "Channels"
                {
                    uniform token info:id = "ND_separate4_color4"
                    color4f inputs:in.connect = </Root/Halo/Image.outputs:out>
                    float outputs:outa
                    float outputs:outb
                    float outputs:outg
                    float outputs:outr
                }
            }
        }
        """
}
