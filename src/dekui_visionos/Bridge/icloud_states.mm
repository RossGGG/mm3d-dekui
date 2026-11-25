// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include "common/file_util.h"
#include "common/logging/log.h"
#include "dekui_visionos/Bridge/icloud_states.h"

namespace Dekui::CloudStates {

namespace {

NSURL* cloud_states;

NSString* ToNS(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

NSDate* ModificationDate(NSURL* url) {
    NSDate* date = nil;
    [url getResourceValue:&date forKey:NSURLContentModificationDateKey error:nil];
    return date;
}

NSNumber* FileSize(NSURL* url) {
    NSNumber* size = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    return size;
}

/// Copies a file into the container. Never replaces anything there.
void Copy(NSURL* local, NSURL* cloud) {
    NSFileManager* files = NSFileManager.defaultManager;
    NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSError* error = nil;
    [coordinator coordinateWritingItemAtURL:cloud
                                    options:0
                                      error:&error
                                 byAccessor:^(NSURL* target) {
                                   NSError* copy_error = nil;
                                   if (![files copyItemAtURL:local toURL:target error:&copy_error]) {
                                       LOG_WARNING(Frontend, "iCloud: could not copy {}: {}",
                                                   local.lastPathComponent.UTF8String,
                                                   copy_error.localizedDescription.UTF8String);
                                   }
                                 }];
    if (error) {
        LOG_WARNING(Frontend, "iCloud: could not write {}: {}", cloud.lastPathComponent.UTF8String,
                    error.localizedDescription.UTF8String);
    }
}

/// Uploads a local state and its thumbnails. States never replace one another:
/// when the container already has a different state of that name (numbered
/// slots filled on two devices), the local one gets a new unique name.
/// Returns false when the state could not be copied.
bool Upload(NSURL* local_state, NSURL* cloud) {
    NSFileManager* files = NSFileManager.defaultManager;
    NSString* name = local_state.lastPathComponent;
    NSURL* target = [cloud URLByAppendingPathComponent:name];
    if ([files fileExistsAtPath:target.path]) {
        const bool same = [FileSize(target) isEqual:FileSize(local_state)] &&
                          [ModificationDate(target) isEqualToDate:ModificationDate(local_state)];
        if (same) {
            return true; // Uploaded before.
        }
        // <title>.<slot>.cst -> <title>.<local time>-<random>.cst
        NSString* title = [name componentsSeparatedByString:@"."].firstObject;
        NSDateFormatter* format = [[NSDateFormatter alloc] init];
        format.dateFormat = @"yyyyMMdd-HHmmss";
        format.timeZone = [NSTimeZone timeZoneWithName:@"UTC"];
        NSString* stamp = [format stringFromDate:ModificationDate(local_state) ?: NSDate.date];
        name = [NSString stringWithFormat:@"%@.%@-%04x.cst", title, stamp, arc4random_uniform(0x10000)];
        target = [cloud URLByAppendingPathComponent:name];
        LOG_INFO(Frontend, "iCloud: kept both copies of {}; this device's is {}",
                 local_state.lastPathComponent.UTF8String, name.UTF8String);
    }
    Copy(local_state, target);
    if (![files fileExistsAtPath:target.path]) {
        return false;
    }
    NSString* local_base = local_state.URLByDeletingPathExtension.path;
    NSString* cloud_base = target.URLByDeletingPathExtension.path;
    for (NSString* suffix in @[ @".jpg", @".sbs.jpg" ]) {
        NSURL* picture = [NSURL fileURLWithPath:[local_base stringByAppendingString:suffix]];
        NSURL* cloud_picture = [NSURL fileURLWithPath:[cloud_base stringByAppendingString:suffix]];
        if ([files fileExistsAtPath:picture.path] && ![files fileExistsAtPath:cloud_picture.path]) {
            Copy(picture, cloud_picture);
        }
    }
    return true;
}

/// Moves an uploaded local state and its thumbnails out of the states folder,
/// so later launches do not upload it again. Kept as a backup.
void Retire(NSURL* local_state, NSURL* backup) {
    NSFileManager* files = NSFileManager.defaultManager;
    [files createDirectoryAtURL:backup withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* base = local_state.URLByDeletingPathExtension.path;
    for (NSString* path in @[ local_state.path, [base stringByAppendingString:@".jpg"],
                              [base stringByAppendingString:@".sbs.jpg"] ]) {
        if (![files fileExistsAtPath:path]) {
            continue;
        }
        NSURL* target = [backup URLByAppendingPathComponent:path.lastPathComponent];
        [files removeItemAtURL:target error:nil];
        [files moveItemAtURL:[NSURL fileURLWithPath:path] toURL:target error:nil];
    }
}

/// The real name of an item, also for an older-style remote-only placeholder
/// (".<name>.icloud").
NSString* LogicalName(NSURL* item) {
    NSString* name = item.lastPathComponent;
    if ([name hasPrefix:@"."] && [name hasSuffix:@".icloud"] && name.length > 8) {
        return [name substringWithRange:NSMakeRange(1, name.length - 8)];
    }
    return name;
}

/// Moves a file within the container, coordinated with iCloud.
bool Move(NSURL* from, NSURL* to) {
    NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSError* error = nil;
    __block bool moved = false;
    [coordinator coordinateWritingItemAtURL:from
                                    options:NSFileCoordinatorWritingForMoving
                           writingItemAtURL:to
                                    options:NSFileCoordinatorWritingForReplacing
                                      error:&error
                                 byAccessor:^(NSURL* source, NSURL* target) {
                                   NSError* move_error = nil;
                                   [coordinator itemAtURL:source willMoveToURL:target];
                                   moved = [NSFileManager.defaultManager moveItemAtURL:source
                                                                                 toURL:target
                                                                                 error:&move_error];
                                   if (moved) {
                                       [coordinator itemAtURL:source didMoveToURL:target];
                                   } else {
                                       LOG_WARNING(Frontend, "iCloud: could not move {}: {}",
                                                   source.lastPathComponent.UTF8String,
                                                   move_error.localizedDescription.UTF8String);
                                   }
                                 }];
    if (error) {
        LOG_WARNING(Frontend, "iCloud: could not move {}: {}", from.lastPathComponent.UTF8String,
                    error.localizedDescription.UTF8String);
    }
    return moved;
}

/// When two devices create the States folder before either has synced it,
/// iCloud renames one ("States 2", "States 3", ...), and the apps, which read
/// only "States", lose sight of its states. Moves them back, thumbnails with
/// their state. Never replaces a file: a different state of the same name
/// (a numbered slot filled on two devices) gets a unique name.
void MergeRenamedFolders(NSURL* cloud) {
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* parent = cloud.URLByDeletingLastPathComponent;
    for (NSURL* folder in [files contentsOfDirectoryAtURL:parent includingPropertiesForKeys:nil
                                                  options:0
                                                    error:nil]) {
        NSString* folder_name = folder.lastPathComponent;
        if (![folder_name hasPrefix:@"States "] ||
            [folder_name substringFromIndex:7].integerValue < 2) {
            continue;
        }
        NSArray<NSURL*>* items = [files contentsOfDirectoryAtURL:folder
                                      includingPropertiesForKeys:@[
                                          NSURLContentModificationDateKey, NSURLFileSizeKey
                                      ]
                                                         options:0
                                                           error:nil];
        // Each state's new base name (unchanged unless it collides).
        NSMutableDictionary<NSString*, NSString*>* bases = [NSMutableDictionary dictionary];
        for (NSURL* item in items) {
            NSString* name = LogicalName(item);
            if (![name.pathExtension.lowercaseString isEqualToString:@"cst"]) {
                continue;
            }
            NSString* base = name.stringByDeletingPathExtension;
            NSURL* existing = [cloud URLByAppendingPathComponent:name];
            if ([files fileExistsAtPath:existing.path]) {
                const bool same = [FileSize(existing) isEqual:FileSize(item)] &&
                                  [ModificationDate(existing) isEqualToDate:ModificationDate(item)];
                if (same) {
                    continue; // Already there; leave this copy where it is.
                }
                NSString* title = [base componentsSeparatedByString:@"."].firstObject;
                bases[base] = [NSString stringWithFormat:@"%@.moved-%04x", title,
                                                         arc4random_uniform(0x10000)];
            } else {
                bases[base] = base;
            }
        }
        unsigned moved = 0;
        for (NSURL* item in items) {
            NSString* name = LogicalName(item);
            NSString* base = nil;
            NSString* suffix = nil;
            for (NSString* candidate in @[ @".sbs.jpg", @".jpg", @".cst" ]) {
                if ([name hasSuffix:candidate]) {
                    base = [name substringToIndex:name.length - candidate.length];
                    suffix = candidate;
                    break;
                }
            }
            NSString* new_base = base ? bases[base] : nil;
            if (!new_base) {
                continue;
            }
            NSURL* source = [folder URLByAppendingPathComponent:name];
            NSURL* target = [cloud URLByAppendingPathComponent:[new_base stringByAppendingString:suffix]];
            if (![files fileExistsAtPath:target.path] && Move(source, target)) {
                ++moved;
            }
        }
        if (moved) {
            LOG_INFO(Frontend, "iCloud: moved {} save state files back from \"{}\"", moved,
                     folder_name.UTF8String);
        }
    }
}

} // namespace

bool Enable() {
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* container = [files URLForUbiquityContainerIdentifier:nil];
#if TARGET_OS_OSX
    // The desktop app is not signed with the iCloud entitlement, so the API
    // finds no container. iCloud Drive still keeps the visionOS app's container
    // in the user's Library, and syncs what is written there.
    if (!container) {
        NSURL* path = [files.homeDirectoryForCurrentUser
            URLByAppendingPathComponent:@"Library/Mobile Documents/iCloud~org~azahar-emu~azahar"
                            isDirectory:YES];
        BOOL directory = NO;
        if ([files fileExistsAtPath:path.path isDirectory:&directory] && directory) {
            container = path;
        }
    }
#endif
    if (!container) {
        LOG_INFO(Frontend, "iCloud Drive unavailable; save states stay on this device");
        return false;
    }
    NSURL* cloud = [container URLByAppendingPathComponent:@"Documents/States" isDirectory:YES];
    NSError* error = nil;
    if (![files createDirectoryAtURL:cloud withIntermediateDirectories:YES attributes:nil
                               error:&error]) {
        LOG_WARNING(Frontend, "iCloud: could not create the States folder: {}",
                    error.localizedDescription.UTF8String);
        return false;
    }

    MergeRenamedFolders(cloud);

    // Existing local states go to iCloud, with their thumbnails. The local
    // copies stay as a backup.
    NSURL* local = [NSURL fileURLWithPath:ToNS(FileUtil::GetUserPath(FileUtil::UserPath::StatesDir))
                              isDirectory:YES];
    for (NSURL* item in [files contentsOfDirectoryAtURL:local
                             includingPropertiesForKeys:@[
                                 NSURLContentModificationDateKey, NSURLFileSizeKey
                             ]
                                                options:NSDirectoryEnumerationSkipsHiddenFiles
                                                  error:nil]) {
        if ([item.pathExtension.lowercaseString isEqualToString:@"cst"] && Upload(item, cloud)) {
            Retire(item, [local URLByAppendingPathComponent:@"Uploaded to iCloud" isDirectory:YES]);
        }
    }

    FileUtil::UpdateUserPath(FileUtil::UserPath::StatesDir, cloud.path.UTF8String);
    cloud_states = cloud;
    LOG_INFO(Frontend, "Save states sync through iCloud: {}", cloud.path.UTF8String);
    DownloadMissing();
    return true;
}

void DownloadMissing() {
    NSURL* cloud = cloud_states;
    if (!cloud) {
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      NSFileManager* files = NSFileManager.defaultManager;
      for (NSURL* item in [files contentsOfDirectoryAtURL:cloud
                               includingPropertiesForKeys:@[
                                   NSURLUbiquitousItemDownloadingStatusKey
                               ]
                                                  options:0
                                                    error:nil]) {
          NSURL* target = item;
          NSString* name = item.lastPathComponent;
          // Older iCloud Drive shows remote-only files as ".<name>.icloud".
          if ([name hasPrefix:@"."] && [name hasSuffix:@".icloud"]) {
              target = [cloud URLByAppendingPathComponent:
                                  [name substringWithRange:NSMakeRange(1, name.length - 8)]];
          } else {
              NSString* status = nil;
              [item getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey
                               error:nil];
              if (!status || [status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]) {
                  continue;
              }
          }
          [files startDownloadingUbiquitousItemAtURL:target error:nil];
      }
    });
}

} // namespace Dekui::CloudStates
