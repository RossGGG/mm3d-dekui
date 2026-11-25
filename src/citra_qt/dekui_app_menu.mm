// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import <AppKit/AppKit.h>

#include <algorithm>

#include "citra_qt/dekui_app_menu.h"

@interface DekuiMenuTarget : NSObject
- (instancetype)initWithAction:(std::function<void()>)action;
- (void)perform:(id)sender;
@end

@implementation DekuiMenuTarget {
    std::function<void()> _action;
}

- (instancetype)initWithAction:(std::function<void()>)action {
    if ((self = [super init])) {
        _action = std::move(action);
    }
    return self;
}

- (void)perform:(id)sender {
    if (_action) {
        _action();
    }
}

@end

namespace Dekui::AppMenu {

void AddItem(const std::string& menu_title, const std::string& title, const std::string& key,
             std::function<void()> action) {
    NSMenu* main_menu = NSApp.mainMenu;
    if (!main_menu) {
        return;
    }
    NSString* name = [NSString stringWithUTF8String:menu_title.c_str()];
    NSMenu* menu = [main_menu itemWithTitle:name].submenu;
    if (!menu) {
        menu = [[NSMenu alloc] initWithTitle:name];
        NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:name action:nil keyEquivalent:@""];
        holder.submenu = menu;
        // Directly after the application menu.
        [main_menu insertItem:holder atIndex:std::min<NSInteger>(1, main_menu.numberOfItems)];
    }

    // Menu items keep their target weakly; keep it alive with the app.
    static NSMutableArray<DekuiMenuTarget*>* targets = [NSMutableArray array];
    DekuiMenuTarget* target = [[DekuiMenuTarget alloc] initWithAction:std::move(action)];
    [targets addObject:target];

    NSMenuItem* item =
        [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:title.c_str()]
                                   action:@selector(perform:)
                            keyEquivalent:[NSString stringWithUTF8String:key.c_str()]];
    item.target = target;
    item.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [menu addItem:item];
}

} // namespace Dekui::AppMenu
