// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import "dekui_visionos/Bridge/DKMotion.h"

#include <algorithm>
#include "dekui_frontend/motion.h"
#include "mm3d/player_settings.h"

namespace {
NSInteger Requested(const std::atomic<int>& request, const std::atomic<int>& current) {
    const int value = request.load();
    return value >= 0 ? value : current.load();
}
} // namespace

@implementation DKMotion

+ (void)load {
#if !TARGET_OS_TV
    // Offers head tracking in the in-game settings too (visionOS only).
    MM3D::PlayerSettings::head_motion_available.store(true);
#endif
}

+ (DKMotionSource)source {
    return DKMotionSource(Requested(MM3D::PlayerSettings::set_motion_source,
                                    MM3D::PlayerSettings::motion_source));
}
+ (void)setSource:(DKMotionSource)source {
    MM3D::PlayerSettings::set_motion_source.store(
        int(std::clamp<NSInteger>(source, DKMotionSourceOff, DKMotionSourceHeadTracking)));
}
+ (NSInteger)sensitivity {
    return Requested(MM3D::PlayerSettings::set_motion_sensitivity,
                     MM3D::PlayerSettings::motion_sensitivity);
}
+ (void)setSensitivity:(NSInteger)sensitivity {
    MM3D::PlayerSettings::set_motion_sensitivity.store(
        int(std::clamp<NSInteger>(sensitivity, 25, 400)));
}

+ (void)setHeadOrientationX:(float)x y:(float)y z:(float)z w:(float)w timestamp:(double)timestamp {
    Dekui::Motion::SetHeadPose(x, y, z, w, timestamp);
}
+ (void)clearHeadPose {
    Dekui::Motion::ClearHeadPose();
}
+ (void)recenterHead {
    Dekui::Motion::RecenterHead();
}

@end
