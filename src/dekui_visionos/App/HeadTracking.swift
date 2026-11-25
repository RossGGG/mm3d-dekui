// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import ARKit
import QuartzCore
import os
import simd

/// Reads the wearer's head pose from ARKit, for head-tracked motion controls
/// (fed to the 3DS motion sensors through DKMotion) and for placing the
/// immersive wide view. ARKit only delivers poses while one of the app's
/// immersive spaces is open; see EmulatorModel.immersiveSpaceWanted.
@MainActor
final class HeadTracker {
    private struct Shared {
        var feedsMotion = false
        var pose: simd_float4x4?
    }

    private var task: Task<Void, Never>?
    private let shared = OSAllocatedUnfairLock(initialState: Shared())

    var running: Bool { task != nil }

    /// Sends the pose to the 3DS motion sensors (head-tracked motion controls).
    var feedsMotion: Bool {
        get { shared.withLock { $0.feedsMotion } }
        set {
            shared.withLock { $0.feedsMotion = newValue }
            if !newValue { DKMotion.clearHeadPose() }
        }
    }

    /// The newest head pose: world from device, in the immersive space's
    /// coordinates.
    var pose: simd_float4x4? { shared.withLock { $0.pose } }

    func start() {
        guard task == nil else { return }
        guard WorldTrackingProvider.isSupported else {
            print("Head tracking is not supported on this device")
            return
        }
        let shared = shared
        // A data provider runs once; each start needs a new session.
        task = Task.detached(priority: .userInitiated) {
            let session = ARKitSession()
            let tracking = WorldTrackingProvider()
            do {
                try await session.run([tracking])
            } catch {
                print("Could not start head tracking: \(error)")
                return
            }
            while !Task.isCancelled {
                let now = CACurrentMediaTime()
                if tracking.state == .running,
                   let anchor = tracking.queryDeviceAnchor(atTimestamp: now), anchor.isTracked {
                    let transform = anchor.originFromAnchorTransform
                    let feedsMotion = shared.withLock { state in
                        state.pose = transform
                        return state.feedsMotion
                    }
                    if feedsMotion {
                        // The world origin is gravity-aligned, y up.
                        let pose = simd_quaternion(transform)
                        DKMotion.setHeadOrientation(x: pose.imag.x, y: pose.imag.y,
                                                    z: pose.imag.z, w: pose.real, timestamp: now)
                    }
                }
                // About the display's rate; HID samples the gyroscope more slowly.
                try? await Task.sleep(for: .milliseconds(8))
            }
            session.stop()
            shared.withLock { $0.pose = nil }
            DKMotion.clearHeadPose()
        }
    }

    func stop() {
        task?.cancel()
        task = nil
        shared.withLock { $0.pose = nil }
        DKMotion.clearHeadPose()
    }
}
