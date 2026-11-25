// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Motion controls (dekui_frontend/motion.h): what drives the 3DS gyroscope
// and accelerometer. Settings apply on the next tick and are saved to
// qt-config.ini.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Settings::MotionSource.
typedef NS_ENUM(NSInteger, DKMotionSource) {
    DKMotionSourceOff,
    /// The game controller's gyroscope (GCMotion through SDL).
    DKMotionSourceController,
    /// The wearer's head pose from ARKit.
    DKMotionSourceHeadTracking,
};

@interface DKMotion : NSObject

@property(class, nonatomic) DKMotionSource source;
/// Gyroscope scale in percent, 25-400.
@property(class, nonatomic) NSInteger sensitivity;

/// The device pose from ARKit's device anchor: a unit quaternion rotating
/// device-local vectors into world space, and the pose's timestamp in
/// seconds. Any thread.
+ (void)setHeadOrientationX:(float)x y:(float)y z:(float)z w:(float)w timestamp:(double)timestamp
    NS_SWIFT_NAME(setHeadOrientation(x:y:z:w:timestamp:));
/// Head tracking stopped; the 3DS lies still until the next pose.
+ (void)clearHeadPose;
/// Makes the current head pose the 3DS's level, resting orientation.
+ (void)recenterHead;

@end

NS_ASSUME_NONNULL_END
