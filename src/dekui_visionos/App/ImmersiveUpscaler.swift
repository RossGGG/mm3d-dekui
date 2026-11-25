// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Metal
#if canImport(MetalFX)
import MetalFX
#endif

/// Final-image scale for the immersive scene. The emulator's internal
/// resolution still controls how much geometry and texture detail is rendered;
/// this controls the texture RealityKit receives.
enum ImmersiveUpscaling: Int, CaseIterable, Identifiable {
    case off
    case oneAndAHalf
    case two

    var id: Int { rawValue }

    var scale: Float {
        switch self {
        case .off: return 1
        case .oneAndAHalf: return 1.5
        case .two: return 2
        }
    }

    var title: String {
        switch self {
        case .off: return "Off"
        case .oneAndAHalf: return "1.5×"
        case .two: return "2×"
        }
    }
}

/// Per-eye FXAA followed by MetalFX spatial upscaling. The source delivered by
/// the emulator is side-by-side in stereo; edge filters must never see the
/// other eye across that texture's middle seam.
#if canImport(MetalFX)
@MainActor
final class ImmersiveUpscaler {
    struct Output {
        let width: Int
        let height: Int
        let pixelFormat: MTLPixelFormat
    }

    private struct Key: Equatable {
        let device: ObjectIdentifier
        let eyeWidth: Int
        let height: Int
        let outputEyeWidth: Int
        let outputHeight: Int
        let pixelFormat: MTLPixelFormat
        let eyes: Int
        let antialiasing: Bool
    }

    private struct FxaaConstants {
        var sourceWidth: UInt32
        var sourceHeight: UInt32
        var eyeWidth: UInt32
        var sourceX: UInt32
    }

    private var key: Key?
    private var failedKey: Key?
    private var inputs: [MTLTexture] = []
    private var outputs: [MTLTexture] = []
    private var scalers: [any MTLFXSpatialScaler] = []
    private var fxaaPipeline: MTLComputePipelineState?
    private var copyPipeline: MTLComputePipelineState?
    private var pipelineDevice: ObjectIdentifier?

    /// Allocates reusable resources and returns the RealityKit texture shape.
    /// Nil means the caller should use its unchanged direct-copy path.
    func prepare(source: MTLTexture, eyes requestedEyes: Int, mode: ImmersiveUpscaling,
                 antialiasing: Bool) -> Output? {
        guard mode != .off, MTLFXSpatialScalerDescriptor.supportsDevice(source.device) else {
            return nil
        }
        let eyes = max(1, min(2, requestedEyes))
        guard source.width % eyes == 0, let pixelFormat = workingFormat(source.pixelFormat) else {
            return nil
        }
        let eyeWidth = source.width / eyes
        let outputEyeWidth = max(eyeWidth + 1, Int((Float(eyeWidth) * mode.scale).rounded()))
        let outputHeight = max(source.height + 1,
                               Int((Float(source.height) * mode.scale).rounded()))
        let wanted = Key(device: ObjectIdentifier(source.device as AnyObject),
                         eyeWidth: eyeWidth, height: source.height,
                         outputEyeWidth: outputEyeWidth, outputHeight: outputHeight,
                         pixelFormat: pixelFormat, eyes: eyes, antialiasing: antialiasing)
        if wanted == failedKey { return nil }
        if wanted != key {
            guard rebuild(for: wanted, device: source.device) else {
                failedKey = wanted
                return nil
            }
        }
        return Output(width: outputEyeWidth * eyes, height: outputHeight,
                      pixelFormat: pixelFormat)
    }

    /// Encodes the prepared per-eye pipeline into `destination`.
    func encode(source: MTLTexture, eyes requestedEyes: Int, destination: MTLTexture,
                antialiasing: Bool, command: MTLCommandBuffer) -> Bool {
        guard let key, key.antialiasing == antialiasing,
              key.eyes == max(1, min(2, requestedEyes)),
              source.width == key.eyeWidth * key.eyes, source.height == key.height,
              destination.width == key.outputEyeWidth * key.eyes,
              destination.height == key.outputHeight,
              destination.pixelFormat == key.pixelFormat,
              inputs.count == key.eyes, outputs.count == key.eyes,
              scalers.count == key.eyes else { return false }

        guard let pipeline = antialiasing ? fxaaPipeline : copyPipeline else { return false }
        for eye in 0..<key.eyes {
            guard let encoder = command.makeComputeCommandEncoder() else { return false }
            encoder.label = antialiasing ? "Dekui immersive FXAA eye \(eye)"
                                         : "Dekui immersive copy eye \(eye)"
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(source, index: 0)
            encoder.setTexture(inputs[eye], index: 1)
            var constants = FxaaConstants(
                sourceWidth: UInt32(source.width), sourceHeight: UInt32(source.height),
                eyeWidth: UInt32(key.eyeWidth), sourceX: UInt32(eye * key.eyeWidth))
            encoder.setBytes(&constants, length: MemoryLayout<FxaaConstants>.stride, index: 0)
            let threads = MTLSize(width: 16, height: 16, depth: 1)
            let grid = MTLSize(width: key.eyeWidth, height: key.height, depth: 1)
            encoder.dispatchThreads(grid, threadsPerThreadgroup: threads)
            encoder.endEncoding()
        }

        for eye in 0..<key.eyes {
            let scaler = scalers[eye]
            scaler.inputContentWidth = key.eyeWidth
            scaler.inputContentHeight = key.height
            scaler.colorTexture = inputs[eye]
            scaler.outputTexture = outputs[eye]
            scaler.encode(commandBuffer: command)
        }

        guard let blit = command.makeBlitCommandEncoder() else { return false }
        for eye in 0..<key.eyes {
            blit.copy(from: outputs[eye], sourceSlice: 0, sourceLevel: 0,
                      sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                      sourceSize: MTLSize(width: key.outputEyeWidth,
                                          height: key.outputHeight, depth: 1),
                      to: destination, destinationSlice: 0, destinationLevel: 0,
                      destinationOrigin: MTLOrigin(x: eye * key.outputEyeWidth, y: 0, z: 0))
        }
        blit.endEncoding()
        return true
    }

    private func rebuild(for key: Key, device: MTLDevice) -> Bool {
        inputs.removeAll()
        outputs.removeAll()
        scalers.removeAll()
        self.key = nil

        if !makePipelines(device: device) { return false }

        for _ in 0..<key.eyes {
            let descriptor = MTLFXSpatialScalerDescriptor()
            descriptor.inputWidth = key.eyeWidth
            descriptor.inputHeight = key.height
            descriptor.outputWidth = key.outputEyeWidth
            descriptor.outputHeight = key.outputHeight
            descriptor.colorTextureFormat = key.pixelFormat
            descriptor.outputTextureFormat = key.pixelFormat
            descriptor.colorProcessingMode = .linear
            guard let scaler = descriptor.makeSpatialScaler(device: device) else { return false }

            let inputDescriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: key.pixelFormat, width: key.eyeWidth, height: key.height,
                mipmapped: false)
            inputDescriptor.storageMode = .private
            inputDescriptor.usage = [.shaderRead, .shaderWrite]
            inputDescriptor.usage.formUnion(scaler.colorTextureUsage)
            let outputDescriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: key.pixelFormat, width: key.outputEyeWidth,
                height: key.outputHeight, mipmapped: false)
            outputDescriptor.storageMode = .private
            outputDescriptor.usage = [.shaderRead, .shaderWrite]
            outputDescriptor.usage.formUnion(scaler.outputTextureUsage)
            guard let input = device.makeTexture(descriptor: inputDescriptor),
                  let output = device.makeTexture(descriptor: outputDescriptor) else { return false }
            input.label = "Dekui immersive upscaler input"
            output.label = "Dekui immersive upscaler output"
            inputs.append(input)
            outputs.append(output)
            scalers.append(scaler)
        }
        self.key = key
        failedKey = nil
        return true
    }

    private func makePipelines(device: MTLDevice) -> Bool {
        let identity = ObjectIdentifier(device as AnyObject)
        if pipelineDevice == identity, fxaaPipeline != nil, copyPipeline != nil { return true }
        do {
            let library = try device.makeLibrary(source: Self.fxaaSource, options: nil)
            guard let fxaa = library.makeFunction(name: "immersive_fxaa"),
                  let copy = library.makeFunction(name: "immersive_copy") else { return false }
            fxaaPipeline = try device.makeComputePipelineState(function: fxaa)
            copyPipeline = try device.makeComputePipelineState(function: copy)
            pipelineDevice = identity
            return true
        } catch {
            NSLog("Dekui: immersive FXAA could not be compiled: %@", "\(error)")
            return false
        }
    }

    private func workingFormat(_ source: MTLPixelFormat) -> MTLPixelFormat? {
        switch source {
        case .bgra8Unorm, .bgra8Unorm_srgb: return .bgra8Unorm
        case .rgba8Unorm, .rgba8Unorm_srgb: return .rgba8Unorm
        default: return nil
        }
    }

    /// FXAA 3.11's compact quality path. Reads are clamped to one eye's source
    /// rectangle so the middle of a side-by-side frame cannot bleed across.
    private static let fxaaSource = """
        #include <metal_stdlib>
        using namespace metal;

        struct FxaaConstants {
            uint sourceWidth;
            uint sourceHeight;
            uint eyeWidth;
            uint sourceX;
        };

        constexpr sampler linearSampler(coord::normalized, address::clamp_to_edge,
                                        filter::linear);

        float3 eyeSample(texture2d<float, access::sample> source, float2 p,
                         constant FxaaConstants& c) {
            p.x = clamp(p.x, 0.5, float(c.eyeWidth) - 0.5) + float(c.sourceX);
            p.y = clamp(p.y, 0.5, float(c.sourceHeight) - 0.5);
            return source.sample(linearSampler,
                p / float2(float(c.sourceWidth), float(c.sourceHeight))).rgb;
        }

        kernel void immersive_copy(texture2d<float, access::sample> source [[texture(0)]],
                                   texture2d<float, access::write> target [[texture(1)]],
                                   constant FxaaConstants& c [[buffer(0)]],
                                   uint2 gid [[thread_position_in_grid]]) {
            if (gid.x >= c.eyeWidth || gid.y >= c.sourceHeight) return;
            float3 colour = eyeSample(source, float2(gid) + 0.5, c);
            target.write(float4(colour, 1.0), gid);
        }

        kernel void immersive_fxaa(texture2d<float, access::sample> source [[texture(0)]],
                                   texture2d<float, access::write> target [[texture(1)]],
                                   constant FxaaConstants& c [[buffer(0)]],
                                   uint2 gid [[thread_position_in_grid]]) {
            if (gid.x >= c.eyeWidth || gid.y >= c.sourceHeight) return;
            float2 p = float2(gid) + 0.5;
            constexpr float3 lumaWeights = float3(0.299, 0.587, 0.114);
            float3 rgbNW = eyeSample(source, p + float2(-1, -1), c);
            float3 rgbNE = eyeSample(source, p + float2( 1, -1), c);
            float3 rgbSW = eyeSample(source, p + float2(-1,  1), c);
            float3 rgbSE = eyeSample(source, p + float2( 1,  1), c);
            float3 rgbM  = eyeSample(source, p, c);
            float lumaNW = dot(rgbNW, lumaWeights);
            float lumaNE = dot(rgbNE, lumaWeights);
            float lumaSW = dot(rgbSW, lumaWeights);
            float lumaSE = dot(rgbSE, lumaWeights);
            float lumaM  = dot(rgbM,  lumaWeights);
            float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
            float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

            float2 dir;
            dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
            dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
            float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 / 8.0),
                               1.0 / 128.0);
            float inverse = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
            dir = clamp(dir * inverse, float2(-8.0), float2(8.0));

            float3 rgbA = 0.5 * (eyeSample(source, p + dir * (-1.0 / 6.0), c) +
                                 eyeSample(source, p + dir * ( 1.0 / 6.0), c));
            float3 rgbB = rgbA * 0.5 + 0.25 * (
                eyeSample(source, p + dir * -0.5, c) +
                eyeSample(source, p + dir *  0.5, c));
            float lumaB = dot(rgbB, lumaWeights);
            float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
            target.write(float4(result, 1.0), gid);
        }
        """
}
#else
/// MetalFX is not present in the visionOS simulator SDK. Keep the ordinary
/// presentation path available there; physical visionOS devices use the
/// implementation above.
@MainActor
final class ImmersiveUpscaler {
    struct Output {
        let width: Int
        let height: Int
        let pixelFormat: MTLPixelFormat
    }

    func prepare(source: MTLTexture, eyes: Int, mode: ImmersiveUpscaling,
                 antialiasing: Bool) -> Output? { nil }

    func encode(source: MTLTexture, eyes: Int, destination: MTLTexture,
                antialiasing: Bool, command: MTLCommandBuffer) -> Bool { false }
}
#endif
