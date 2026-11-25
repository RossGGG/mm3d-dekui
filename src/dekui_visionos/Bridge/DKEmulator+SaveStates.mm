// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import "dekui_visionos/Bridge/DKEmulator+SaveStates.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <memory>
#include <vector>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "dekui_visionos/Bridge/cloudkit_states.h"
#include "dekui_visionos/Bridge/icloud_states.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace {

constexpr size_t ThumbnailWidth = 480;

/// Writes a downscaled JPEG of a BGRA frame from the renderer's screenshot.
void WriteThumbnail(const std::vector<u8>& pixels, u32 width, u32 height, bool invert_y,
                    bool side_by_side, const std::string& path) {
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    CGContextRef source = CGBitmapContextCreate(
        const_cast<u8*>(pixels.data()), width, height, 8, width * 4, color_space,
        kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    CGImageRef eye = source ? CGBitmapContextCreateImage(source) : nullptr;
    // With side-by-side 3D the frame holds both eyes; keep both, each eye
    // ThumbnailWidth wide.
    const size_t eye_width = side_by_side ? width / 2 : width;
    const size_t thumb_eye_width = std::min(ThumbnailWidth, eye_width);
    const size_t thumb_width = side_by_side ? thumb_eye_width * 2 : thumb_eye_width;
    const size_t thumb_height =
        std::max<size_t>(1, height * thumb_eye_width / std::max<size_t>(1, eye_width));
    CGContextRef thumb = CGBitmapContextCreate(nullptr, thumb_width, thumb_height, 8, 0, color_space,
                                               kCGBitmapByteOrder32Little |
                                                   kCGImageAlphaNoneSkipFirst);
    if (eye && thumb) {
        CGContextSetInterpolationQuality(thumb, kCGInterpolationHigh);
        if (invert_y) {
            CGContextTranslateCTM(thumb, 0, thumb_height);
            CGContextScaleCTM(thumb, 1, -1);
        }
        CGContextDrawImage(thumb, CGRectMake(0, 0, thumb_width, thumb_height), eye);
        CGImageRef image = CGBitmapContextCreateImage(thumb);
        // Written beside and renamed, so iCloud never syncs half a picture.
        const std::string temporary = path + ".partial";
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:temporary.c_str()]];
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
            (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
        if (destination && image) {
            NSDictionary* options = @{(__bridge NSString*)kCGImageDestinationLossyCompressionQuality : @0.8};
            CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)options);
            if (CGImageDestinationFinalize(destination)) {
                FileUtil::Rename(temporary, path);
            }
        }
        if (destination) {
            CFRelease(destination);
        }
        CGImageRelease(image);
    }
    CGContextRelease(thumb);
    CGImageRelease(eye);
    CGContextRelease(source);
    CGColorSpaceRelease(color_space);
}

/// Asks the renderer for the next frame, as the window shows it, and saves a
/// thumbnail beside the state (Core::SaveStateThumbnailPath).
void CaptureThumbnail(Core::System& system, const std::string& state_path) {
    auto& renderer = system.GPU().Renderer();
    const Layout::FramebufferLayout layout = renderer.GetRenderWindow().GetFramebufferLayout();
    if (layout.width == 0 || layout.height == 0) {
        return;
    }
    auto pixels = std::make_shared<std::vector<u8>>(size_t(layout.width) * layout.height * 4);
    const bool side_by_side =
        Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::SideBySideFull ||
        Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::SideBySide;
    const std::string path = Core::SaveStateThumbnailPath(state_path, side_by_side);
    // Only one kind of thumbnail per state.
    FileUtil::Delete(Core::SaveStateThumbnailPath(state_path, !side_by_side));
    const u32 width = layout.width, height = layout.height;
    renderer.RequestScreenshot(
        pixels->data(),
        [pixels, width, height, side_by_side, path](bool invert_y) {
            // Called on the render thread; encode elsewhere.
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
              WriteThumbnail(*pixels, width, height, invert_y, side_by_side, path);
            });
        },
        layout);
}

} // namespace

@implementation DKSaveState {
    NSString* _path;
    NSInteger _slot;
    NSDate* _date;
    NSString* _incompatibility;
    NSURL* _thumbnail;
    BOOL _thumbnailIsStereo;
}

- (instancetype)initWithPath:(NSString*)path
                        slot:(NSInteger)slot
                        date:(NSDate*)date
             incompatibility:(nullable NSString*)incompatibility
                   thumbnail:(nullable NSURL*)thumbnail
                      stereo:(BOOL)stereo {
    if ((self = [super init])) {
        _path = path;
        _slot = slot;
        _date = date;
        _incompatibility = incompatibility;
        _thumbnail = thumbnail;
        _thumbnailIsStereo = stereo;
    }
    return self;
}

- (NSString*)path {
    return _path;
}

- (NSInteger)slot {
    return _slot;
}

- (NSDate*)date {
    return _date;
}

- (nullable NSString*)incompatibility {
    return _incompatibility;
}

- (nullable NSURL*)thumbnail {
    return _thumbnail;
}

- (BOOL)thumbnailIsStereo {
    return _thumbnailIsStereo;
}

@end

namespace {

u64 TitleId(Core::System& system) {
    u64 title_id{};
    system.GetAppLoader().ReadProgramId(title_id);
    return title_id;
}

NSString* ToNS(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

} // namespace

@implementation DKEmulator (SaveStates)

- (NSArray<DKSaveState*>*)saveStates {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return @[];
    }
    NSMutableArray<DKSaveState*>* states = [NSMutableArray array];
    for (const auto& file :
         Core::ListSaveStateFiles(TitleId(system), system.Movie().GetCurrentMovieID())) {
        NSString* incompatibility = nil;
        if (file.info.status == Core::SaveStateInfo::ValidationStatus::BuildMismatch) {
            incompatibility = [NSString stringWithFormat:@"Made by %s", file.info.build_name.c_str()];
        }
        const auto stereo_path = Core::SaveStateThumbnailPath(file.path, true);
        const bool stereo = FileUtil::Exists(stereo_path);
        const auto thumbnail_path =
            stereo ? stereo_path : Core::SaveStateThumbnailPath(file.path, false);
        NSURL* thumbnail = FileUtil::Exists(thumbnail_path)
                               ? [NSURL fileURLWithPath:ToNS(thumbnail_path)]
                               : nil;
        [states addObject:[[DKSaveState alloc]
                              initWithPath:ToNS(file.path)
                                      slot:file.slot ? NSInteger(*file.slot) : -1
                                      date:[NSDate dateWithTimeIntervalSince1970:
                                                       static_cast<double>(file.info.time)]
                           incompatibility:incompatibility
                                 thumbnail:thumbnail
                                    stereo:stereo]];
    }
    return states;
}

- (void)downloadCloudSaveStates {
    Dekui::CloudStates::DownloadMissing();
    Dekui::CloudKitStates::Sync();
}

- (BOOL)requestStateFile:(Core::System::Signal)signal path:(const std::string&)path {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn() || !system.SendFileSignal(signal, path)) {
        return NO;
    }
    // Requests are handled inside RunLoop, which a paused game does not call.
    [self resume];
    return YES;
}

- (nullable NSString*)saveNewState {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return nil;
    }
    const std::string path =
        Core::NewSaveStatePath(TitleId(system), system.Movie().GetCurrentMovieID());
    if (![self requestStateFile:Core::System::Signal::SaveFile path:path]) {
        return nil;
    }
    CaptureThumbnail(system, path);
    // CloudKit uploads states once they have settled (written and thumbnailed).
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      Dekui::CloudKitStates::Sync();
    });
    return ToNS(path);
}

- (BOOL)loadState:(DKSaveState*)state {
    return [self requestStateFile:Core::System::Signal::LoadFile path:state.path.UTF8String];
}

- (BOOL)loadLatestState {
    for (DKSaveState* state in [self saveStates]) {
        if (!state.incompatibility) {
            return [self loadState:state];
        }
    }
    return NO;
}

- (BOOL)deleteState:(DKSaveState*)state {
    const bool deleted = Core::DeleteSaveStateFile(state.path.UTF8String);
    Dekui::CloudKitStates::Sync();
    return deleted ? YES : NO;
}

- (void)captureDebugFrame {
    if (self.state != DKEmulationStateRunning && self.state != DKEmulationStatePaused) {
        return;
    }
    CaptureThumbnail(Core::System::GetInstance(),
                     FileUtil::GetUserPath(FileUtil::UserPath::LogDir) + "debug-frame.cst");
}

@end
