// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import <CloudKit/CloudKit.h>
#import <Foundation/Foundation.h>

#include <map>
#include <string>

#include "common/file_util.h"
#include "common/logging/log.h"
#include "dekui_visionos/Bridge/cloudkit_states.h"

namespace Dekui::CloudKitStates {

namespace {

NSString* const ContainerId = @"iCloud.org.azahar-emu.azahar";
NSString* const RecordType = @"SaveState";
/// A state file younger than this may still be being written.
constexpr NSTimeInterval SettleSeconds = 10;

dispatch_queue_t queue;
CKDatabase* database;
CKRecordZoneID* zone;
NSURL* folder;
/// Names known to be on both sides after the last sync (per device).
NSURL* ledger_url;
NSMutableSet<NSString*>* ledger;
bool zone_ready = false;
bool syncing = false;
bool sync_again = false;

NSString* ToNS(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

void SaveLedger() {
    [ledger.allObjects writeToURL:ledger_url error:nil];
}

/// The state's real name, also for an older-style iCloud Drive placeholder
/// (".<name>.icloud").
NSString* LogicalName(NSString* name) {
    if ([name hasPrefix:@"."] && [name hasSuffix:@".icloud"] && name.length > 8) {
        return [name substringWithRange:NSMakeRange(1, name.length - 8)];
    }
    return name;
}

struct LocalState {
    NSURL* url;
    /// Downloaded (not an iCloud Drive placeholder) and fully written.
    bool readable;
};

void ListLocal(std::map<std::string, LocalState>& out) {
    NSFileManager* files = NSFileManager.defaultManager;
    NSArray<NSURLResourceKey>* keys = @[
        NSURLContentModificationDateKey, NSURLUbiquitousItemDownloadingStatusKey
    ];
    for (NSURL* item in [files contentsOfDirectoryAtURL:folder
                             includingPropertiesForKeys:keys
                                                options:0
                                                  error:nil]) {
        NSString* name = LogicalName(item.lastPathComponent);
        if (![name.pathExtension.lowercaseString isEqualToString:@"cst"]) {
            continue;
        }
        NSDate* modified = nil;
        NSString* status = nil;
        [item getResourceValue:&modified forKey:NSURLContentModificationDateKey error:nil];
        [item getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
        const bool placeholder = ![item.lastPathComponent isEqualToString:name] ||
                                 (status && ![status isEqualToString:
                                                         NSURLUbiquitousItemDownloadingStatusCurrent]);
        const bool settled = modified && -modified.timeIntervalSinceNow > SettleSeconds;
        out[name.UTF8String] = {[folder URLByAppendingPathComponent:name], !placeholder && settled};
    }
}

/// The thumbnail beside a state, if any, and whether it holds both eyes.
NSURL* Thumbnail(NSURL* state, bool* stereo) {
    NSString* base = state.URLByDeletingPathExtension.path;
    for (NSString* suffix in @[ @".sbs.jpg", @".jpg" ]) {
        NSString* path = [base stringByAppendingString:suffix];
        if ([NSFileManager.defaultManager fileExistsAtPath:path]) {
            *stereo = [suffix isEqualToString:@".sbs.jpg"];
            return [NSURL fileURLWithPath:path];
        }
    }
    return nil;
}

/// A private copy for CKAsset: the original may change or move while the
/// upload runs (iCloud Drive, a new save into the same slot).
NSURL* TemporaryCopy(NSURL* url) {
    NSURL* copy = [[NSURL fileURLWithPath:NSTemporaryDirectory()]
        URLByAppendingPathComponent:[NSUUID.UUID.UUIDString
                                        stringByAppendingPathExtension:url.pathExtension]];
    NSError* error = nil;
    if (![NSFileManager.defaultManager copyItemAtURL:url toURL:copy error:&error]) {
        LOG_WARNING(Frontend, "CloudKit: could not read {}: {}", url.lastPathComponent.UTF8String,
                    error.localizedDescription.UTF8String);
        return nil;
    }
    return copy;
}

/// Writes a downloaded file into the states folder, coordinated with iCloud
/// Drive when the folder is in it. Written beside and renamed, so a partial
/// file never shows up as a state.
bool Place(NSURL* source, NSURL* target) {
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* partial = [target URLByAppendingPathExtension:@"partial"];
    [files removeItemAtURL:partial error:nil];
    NSError* error = nil;
    if (![files copyItemAtURL:source toURL:partial error:&error]) {
        LOG_WARNING(Frontend, "CloudKit: could not write {}: {}",
                    target.lastPathComponent.UTF8String, error.localizedDescription.UTF8String);
        return false;
    }
    __block bool placed = false;
    NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateWritingItemAtURL:target
                                    options:NSFileCoordinatorWritingForReplacing
                                      error:&error
                                 byAccessor:^(NSURL* destination) {
                                   [files removeItemAtURL:destination error:nil];
                                   placed = [files moveItemAtURL:partial toURL:destination
                                                           error:nil];
                                 }];
    if (!placed) {
        [files removeItemAtURL:partial error:nil];
    }
    return placed;
}

void DeleteLocal(NSURL* state) {
    NSString* base = state.URLByDeletingPathExtension.path;
    for (NSString* path in @[ state.path, [base stringByAppendingString:@".jpg"],
                              [base stringByAppendingString:@".sbs.jpg"] ]) {
        if (!FileUtil::Exists(path.UTF8String)) {
            continue;
        }
        NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        [coordinator coordinateWritingItemAtURL:[NSURL fileURLWithPath:path]
                                        options:NSFileCoordinatorWritingForDeleting
                                          error:nil
                                     byAccessor:^(NSURL* target) {
                                       [NSFileManager.defaultManager removeItemAtURL:target
                                                                               error:nil];
                                     }];
    }
}

void Finish();

/// Runs `steps` one after another on the sync queue; each calls `next` when
/// its CloudKit request completes.
void RunSteps(NSMutableArray<void (^)(dispatch_block_t)>* steps) {
    if (steps.count == 0) {
        Finish();
        return;
    }
    void (^step)(dispatch_block_t) = steps.firstObject;
    [steps removeObjectAtIndex:0];
    step(^{
      dispatch_async(queue, ^{
        RunSteps(steps);
      });
    });
}

void Upload(NSString* name, NSURL* url, dispatch_block_t next) {
    NSURL* state_copy = TemporaryCopy(url);
    if (!state_copy) {
        next();
        return;
    }
    bool stereo = false;
    NSURL* thumbnail = Thumbnail(url, &stereo);
    NSURL* thumbnail_copy = thumbnail ? TemporaryCopy(thumbnail) : nil;
    CKRecordID* record_id = [[CKRecordID alloc] initWithRecordName:name zoneID:zone];
    CKRecord* record = [[CKRecord alloc] initWithRecordType:RecordType recordID:record_id];
    record[@"state"] = [[CKAsset alloc] initWithFileURL:state_copy];
    if (thumbnail_copy) {
        record[@"thumbnail"] = [[CKAsset alloc] initWithFileURL:thumbnail_copy];
        record[@"thumbnailStereo"] = @(stereo ? 1 : 0);
    }
    [database saveRecord:record
        completionHandler:^(CKRecord* saved, NSError* error) {
          [NSFileManager.defaultManager removeItemAtURL:state_copy error:nil];
          if (thumbnail_copy) {
              [NSFileManager.defaultManager removeItemAtURL:thumbnail_copy error:nil];
          }
          dispatch_async(queue, ^{
            if (error && error.code != CKErrorServerRecordChanged) {
                LOG_WARNING(Frontend, "CloudKit: could not upload {}: {}", name.UTF8String,
                            error.localizedDescription.UTF8String);
            } else {
                LOG_INFO(Frontend, "CloudKit: uploaded {}", name.UTF8String);
                [ledger addObject:name];
                SaveLedger();
            }
            next();
          });
        }];
}

void Download(NSString* name, dispatch_block_t next) {
    CKRecordID* record_id = [[CKRecordID alloc] initWithRecordName:name zoneID:zone];
    [database fetchRecordWithID:record_id
              completionHandler:^(CKRecord* record, NSError* error) {
                // The asset files only live until this handler returns.
                bool placed = false;
                if (record) {
                    CKAsset* state = record[@"state"];
                    NSURL* target = [folder URLByAppendingPathComponent:name];
                    placed = state.fileURL && Place(state.fileURL, target);
                    CKAsset* thumbnail = record[@"thumbnail"];
                    if (placed && thumbnail.fileURL) {
                        const bool stereo = [record[@"thumbnailStereo"] integerValue] != 0;
                        NSString* base = target.URLByDeletingPathExtension.path;
                        Place(thumbnail.fileURL,
                              [NSURL fileURLWithPath:[base stringByAppendingString:
                                                               stereo ? @".sbs.jpg" : @".jpg"]]);
                    }
                }
                dispatch_async(queue, ^{
                  if (placed) {
                      LOG_INFO(Frontend, "CloudKit: downloaded {}", name.UTF8String);
                      [ledger addObject:name];
                      SaveLedger();
                  } else {
                      LOG_WARNING(Frontend, "CloudKit: could not download {}: {}", name.UTF8String,
                                  error ? error.localizedDescription.UTF8String : "no data");
                  }
                  next();
                });
              }];
}

void DeleteRemote(NSString* name, dispatch_block_t next) {
    CKRecordID* record_id = [[CKRecordID alloc] initWithRecordName:name zoneID:zone];
    [database deleteRecordWithID:record_id
               completionHandler:^(CKRecordID*, NSError* error) {
                 dispatch_async(queue, ^{
                   if (error && error.code != CKErrorUnknownItem) {
                       LOG_WARNING(Frontend, "CloudKit: could not delete {}: {}", name.UTF8String,
                                   error.localizedDescription.UTF8String);
                   } else {
                       LOG_INFO(Frontend, "CloudKit: deleted {} (deleted on this device)",
                                name.UTF8String);
                       [ledger removeObject:name];
                       SaveLedger();
                   }
                   next();
                 });
               }];
}

/// Compares both sides and queues the transfers. A remote listing that
/// failed never counts as "everything was deleted".
void Reconcile(NSSet<NSString*>* remote) {
    std::map<std::string, LocalState> local;
    ListLocal(local);
    NSMutableArray<void (^)(dispatch_block_t)>* steps = [NSMutableArray array];
    for (const auto& [key, state] : local) {
        NSString* name = ToNS(key);
        NSURL* url = state.url;
        if ([remote containsObject:name]) {
            [ledger addObject:name];
        } else if ([ledger containsObject:name]) {
            // Was on both sides; another device deleted it.
            LOG_INFO(Frontend, "CloudKit: removing {} (deleted on another device)", name.UTF8String);
            DeleteLocal(url);
            [ledger removeObject:name];
        } else if (state.readable) {
            [steps addObject:^(dispatch_block_t next) {
              Upload(name, url, next);
            }];
        }
    }
    for (NSString* name in remote) {
        if (local.contains(name.UTF8String)) {
            continue;
        }
        if ([ledger containsObject:name]) {
            [steps addObject:^(dispatch_block_t next) {
              DeleteRemote(name, next);
            }];
        } else {
            [steps addObject:^(dispatch_block_t next) {
              Download(name, next);
            }];
        }
    }
    SaveLedger();
    RunSteps(steps);
}

void FetchRemote() {
    NSMutableSet<NSString*>* remote = [NSMutableSet set];
    CKFetchRecordZoneChangesConfiguration* configuration =
        [[CKFetchRecordZoneChangesConfiguration alloc] init];
    // Names only; assets download when a state is missing here.
    configuration.desiredKeys = @[ @"thumbnailStereo" ];
    CKFetchRecordZoneChangesOperation* operation = [[CKFetchRecordZoneChangesOperation alloc]
        initWithRecordZoneIDs:@[ zone ]
        configurationsByRecordZoneID:@{zone : configuration}];
    operation.fetchAllChanges = YES;
    operation.recordWasChangedBlock = ^(CKRecordID* record_id, CKRecord* record, NSError* error) {
      if (record) {
          dispatch_async(queue, ^{
            [remote addObject:record_id.recordName];
          });
      }
    };
    operation.fetchRecordZoneChangesCompletionBlock = ^(NSError* error) {
      dispatch_async(queue, ^{
        if (error) {
            LOG_WARNING(Frontend, "CloudKit: could not list save states: {}",
                        error.localizedDescription.UTF8String);
            Finish();
            return;
        }
        Reconcile(remote);
      });
    };
    [database addOperation:operation];
}

void Begin() {
    if (zone_ready) {
        FetchRemote();
        return;
    }
    CKRecordZone* record_zone = [[CKRecordZone alloc] initWithZoneID:zone];
    [database saveRecordZone:record_zone
           completionHandler:^(CKRecordZone*, NSError* error) {
             dispatch_async(queue, ^{
               if (error) {
                   LOG_WARNING(Frontend, "CloudKit: save states unavailable: {}",
                               error.localizedDescription.UTF8String);
                   Finish();
                   return;
               }
               zone_ready = true;
               FetchRemote();
             });
           }];
}

void Finish() {
    syncing = false;
    if (sync_again) {
        sync_again = false;
        syncing = true;
        Begin();
    }
}

} // namespace

void Start() {
    if (queue) {
        return;
    }
    queue = dispatch_queue_create("Dekui.CloudKitStates", DISPATCH_QUEUE_SERIAL);
    folder = [NSURL fileURLWithPath:ToNS(FileUtil::GetUserPath(FileUtil::UserPath::StatesDir))
                        isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
    ledger_url = [NSURL
        fileURLWithPath:ToNS(FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) +
                             "cloudkit-states.plist")];
    ledger = [NSMutableSet setWithArray:[NSArray arrayWithContentsOfURL:ledger_url] ?: @[]];
    CKContainer* container = [CKContainer containerWithIdentifier:ContainerId];
    zone = [[CKRecordZoneID alloc] initWithZoneName:@"SaveStates"
                                          ownerName:CKCurrentUserDefaultName];
    [container accountStatusWithCompletionHandler:^(CKAccountStatus status, NSError* error) {
      dispatch_async(queue, ^{
        if (status != CKAccountStatusAvailable) {
            LOG_INFO(Frontend, "CloudKit: no iCloud account (status {}); save states stay here",
                     static_cast<int>(status));
            return;
        }
        // Set only now: Sync() does nothing until the account is known.
        database = container.privateCloudDatabase;
        LOG_INFO(Frontend, "CloudKit: syncing save states in {}", folder.path.UTF8String);
        syncing = true;
        Begin();
      });
    }];
}

void Sync() {
    if (!queue) {
        return;
    }
    dispatch_async(queue, ^{
      if (!database) {
          return;
      }
      if (syncing) {
          sync_again = true;
          return;
      }
      syncing = true;
      Begin();
    });
}

} // namespace Dekui::CloudKitStates
