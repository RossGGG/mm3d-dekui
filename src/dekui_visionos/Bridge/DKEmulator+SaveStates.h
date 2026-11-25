// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import "DKEmulator.h"

NS_ASSUME_NONNULL_BEGIN

/// One save state file. New states each get their own file, so states saved
/// on different devices never overwrite each other. States in the desktop
/// app's numbered slots (its hotkeys) are listed too.
@interface DKSaveState : NSObject
/// Identifies the state; pass it back to load or delete it.
@property(nonatomic, readonly) NSString* path;
/// The numbered slot for slot files, -1 for other states. Slot 0 is the
/// desktop app's Quick Save.
@property(nonatomic, readonly) NSInteger slot;
@property(nonatomic, readonly) NSDate* date;
/// Set when the state was made by a different emulator build and cannot load.
@property(nonatomic, readonly, nullable) NSString* incompatibility;
/// A picture of the game when the state was saved, if one was captured.
@property(nonatomic, readonly, nullable) NSURL* thumbnail;
/// The thumbnail holds both eyes side by side (saved with 3D on).
@property(nonatomic, readonly) BOOL thumbnailIsStereo;
@end

@interface DKEmulator (SaveStates)

/// The running game's save states, newest first. Empty when no game runs.
- (NSArray<DKSaveState*>*)saveStates;

/// Starts downloading save states and thumbnails that iCloud has not
/// delivered yet, such as ones saved on another device.
- (void)downloadCloudSaveStates;

/// Saves the game to a new state file and returns its path, or nil when
/// another save state operation is still pending. The emulation thread does
/// the work over the next frames, so a paused game is resumed first. A
/// thumbnail of the current frame is stored beside it.
- (nullable NSString*)saveNewState;

/// Loads a state. Returns NO when another operation is still pending.
- (BOOL)loadState:(DKSaveState*)state;

/// Loads the newest state this build can load. Returns NO when there is none
/// or another operation is still pending.
- (BOOL)loadLatestState;

/// Deletes a state and its thumbnail, from every device that syncs it.
- (BOOL)deleteState:(DKSaveState*)state;

/// Writes a picture of the next frame to user/log (debug-frame.jpg), as a
/// save state thumbnail would show it. For checking rendering remotely.
- (void)captureDebugFrame;

@end

NS_ASSUME_NONNULL_END
