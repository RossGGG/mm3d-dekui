// Copyright 2025 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#elif !TARGET_OS_VISION
#import <UIKit/UIKit.h>
#endif

namespace AppleUtils {

float GetRefreshRate() { // TODO: How does this handle multi-monitor? -OS
#if TARGET_OS_OSX
    NSScreen* screen = [NSScreen mainScreen];
    if (screen) {
        NSDictionary* screenInfo = [screen deviceDescription];
        CGDirectDisplayID displayID =
            (CGDirectDisplayID)[screenInfo[@"NSScreenNumber"] unsignedIntValue];
        CGDisplayModeRef displayMode = CGDisplayCopyDisplayMode(displayID);
        if (displayMode) {
            CGFloat refreshRate = CGDisplayModeGetRefreshRate(displayMode);
            CFRelease(displayMode);
            return refreshRate;
        }
    }
#elif TARGET_OS_VISION
    // UIScreen is unavailable on visionOS. Vision Pro's default immersive
    // refresh rate is 90 Hz; use it as the frame-pacing baseline until the
    // compositor exposes a per-window refresh-rate API to native clients.
    return 90;
#else
    UIScreen* screen = [UIScreen mainScreen];
    if (screen) {
        return static_cast<float>(screen.maximumFramesPerSecond);
    }
#endif

    return 60; // Something went wrong, so just return a generic value
}

int IsLowPowerModeEnabled() {
    return (int)[NSProcessInfo processInfo].lowPowerModeEnabled;
}

} // namespace AppleUtils
