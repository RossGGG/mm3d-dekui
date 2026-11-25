// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#import <Foundation/Foundation.h>

#include "common/file_util.h"
#include "common/logging/log.h"
#include "dekui_visionos/Bridge/icloud_shader_cache.h"

namespace Dekui::CloudShaderCache {

namespace {
NSString* ToNS(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()];
}
} // namespace

void Fetch() {
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* container = [files URLForUbiquityContainerIdentifier:nil];
    if (!container) {
        return;
    }
    NSURL* cloud = [container URLByAppendingPathComponent:@"Documents/Shader Cache"
                                              isDirectory:YES];
    // Create the folder so it shows up in iCloud Drive for the Mac tour to fill.
    [files createDirectoryAtURL:cloud withIntermediateDirectories:YES attributes:nil error:nil];

    NSURL* import = [NSURL
        fileURLWithPath:ToNS(FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "import")
            isDirectory:YES];
    NSURL* ledger_url = [NSURL
        fileURLWithPath:ToNS(FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) +
                             "shader-cache-import.plist")];
    NSMutableDictionary* ledger =
        [NSMutableDictionary dictionaryWithContentsOfURL:ledger_url] ?: [NSMutableDictionary new];

    NSArray<NSURL*>* items =
        [files contentsOfDirectoryAtURL:cloud
             includingPropertiesForKeys:@[ NSURLUbiquitousItemDownloadingStatusKey,
                                           NSURLContentModificationDateKey, NSURLFileSizeKey ]
                                options:0
                                  error:nil];
    unsigned copied = 0;
    for (NSURL* item in items) {
        NSString* name = item.lastPathComponent;
        // Not yet downloaded files can appear as ".<name>.icloud" placeholders.
        if ([name hasPrefix:@"."] && [name hasSuffix:@".icloud"]) {
            NSString* real = [name substringWithRange:NSMakeRange(1, name.length - 8)];
            [files startDownloadingUbiquitousItemAtURL:[cloud URLByAppendingPathComponent:real]
                                                 error:nil];
            continue;
        }
        if (![name.pathExtension isEqualToString:@"vkch"]) {
            continue;
        }
        NSString* status = nil;
        [item getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
        if (status && ![status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]) {
            [files startDownloadingUbiquitousItemAtURL:item error:nil];
            LOG_INFO(Frontend, "Shader cache: downloading {} from iCloud", name.UTF8String);
            continue;
        }
        NSDate* date = nil;
        NSNumber* size = nil;
        [item getResourceValue:&date forKey:NSURLContentModificationDateKey error:nil];
        [item getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        NSString* version =
            [NSString stringWithFormat:@"%@ %f", size, date.timeIntervalSince1970];
        if ([ledger[name] isEqualToString:version]) {
            continue;
        }
        [files createDirectoryAtURL:import withIntermediateDirectories:YES attributes:nil
                              error:nil];
        NSURL* target = [import URLByAppendingPathComponent:name];
        [files removeItemAtURL:target error:nil];
        NSError* error = nil;
        if (![files copyItemAtURL:item toURL:target error:&error]) {
            LOG_WARNING(Frontend, "Shader cache: could not copy {} from iCloud: {}",
                        name.UTF8String, error.localizedDescription.UTF8String);
            continue;
        }
        ledger[name] = version;
        copied++;
    }
    if (copied) {
        [ledger writeToURL:ledger_url atomically:YES];
        LOG_INFO(Frontend, "Shader cache: {} files from iCloud will be merged at boot", copied);
    }
}

} // namespace Dekui::CloudShaderCache
