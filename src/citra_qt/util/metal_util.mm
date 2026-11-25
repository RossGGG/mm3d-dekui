// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#import <QuartzCore/CAMetalLayer.h>
#import <qwindowdefs.h>

namespace MetalUtil {

#if !TARGET_OS_OSX
static CGFloat MetalContentsScale(UIView* view) {
#if defined(TARGET_OS_VISION) && TARGET_OS_VISION
    // Present the emulator's final composite at window resolution. Rendering
    // at the headset's UIKit backing scale wastes fill rate without adding
    // source detail, since the 3DS image has already been upscaled.
    return 1.0;
#else
    return view.contentScaleFactor;
#endif
}
#endif

void* CreateMetalLayer(WId winId) {
    // Qt's MetalSurface QWindow does not always install a CAMetalLayer before
    // MoltenVK queries the surface extent. Force one onto the native view.
    CAMetalLayer* metalLayer = [CAMetalLayer layer];
#if TARGET_OS_OSX
    NSView* view = (__bridge NSView*)winId;
    view.wantsLayer = YES;
    view.layer = metalLayer;
#else
    UIView* view = (__bridge UIView*)winId;
    [view.layer addSublayer:metalLayer];
    metalLayer.frame = view.bounds;
    metalLayer.contentsScale = MetalContentsScale(view);
    view.layer.masksToBounds = YES;
#endif
    return (__bridge void*)metalLayer;
}

void ResizeMetalLayer(void* layer, WId winId) {
    CAMetalLayer* metalLayer = (__bridge CAMetalLayer*)layer;
#if TARGET_OS_OSX
    NSView* view = (__bridge NSView*)winId;
    metalLayer.frame = view.bounds;
#else
    UIView* view = (__bridge UIView*)winId;
    // Qt creates the native view at its minimum size, then lays it out after the
    // visionOS window appears. CALayer autoresizing is unavailable on visionOS,
    // so follow the host view explicitly from Qt's resize event.
    metalLayer.frame = view.bounds;
    metalLayer.contentsScale = MetalContentsScale(view);
#endif
}

} // namespace MetalUtil
