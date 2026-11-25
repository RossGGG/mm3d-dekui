// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The Swift-facing emulator API. Every method is called on the main thread;
// host callbacks are delivered on the main thread.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, DKSetupStatus) {
    DKSetupStatusReady,
    DKSetupStatusMissingGame,
    DKSetupStatusFailed,
};

typedef NS_ENUM(NSInteger, DKEmulationState) {
    DKEmulationStateStopped,
    DKEmulationStateLoading,
    DKEmulationStateRunning,
    DKEmulationStatePaused,
};

/// 3DS controls, in Settings::NativeButton order, then the two analog sticks.
typedef NS_ENUM(NSInteger, DKControl) {
    DKControlButtonA, DKControlButtonB, DKControlButtonX, DKControlButtonY,
    DKControlDPadUp, DKControlDPadDown, DKControlDPadLeft, DKControlDPadRight,
    DKControlButtonL, DKControlButtonR, DKControlButtonStart, DKControlButtonSelect,
    DKControlButtonDebug, DKControlButtonGPIO14, DKControlButtonZL, DKControlButtonZR,
    DKControlButtonHome, DKControlButtonPower,
    DKControlCirclePad, DKControlCStick,
};

typedef NS_ENUM(NSInteger, DKStickDirection) {
    DKStickDirectionUp, DKStickDirectionDown, DKStickDirectionLeft, DKStickDirectionRight,
};

/// Controller input for moving through Game Options without a pointer.
typedef NS_ENUM(NSInteger, DKOverlayInput) {
    DKOverlayInputUp, DKOverlayInputDown, DKOverlayInputLeft, DKOverlayInputRight,
    /// Presses the focused item.
    DKOverlayInputActivate,
    /// Leaves a field or popup.
    DKOverlayInputCancel,
    /// Previous and next tab or window.
    DKOverlayInputPrevious, DKOverlayInputNext,
};

@protocol DKEmulatorHost <NSObject>
- (void)emulatorStateChanged:(DKEmulationState)state;
- (void)emulatorLoadProgress:(double)fraction message:(NSString*)message;
- (void)emulatorFailed:(NSString*)title message:(NSString*)message;
- (void)emulatorShowControls;
- (void)emulatorShowLicenses:(NSString*)path;
- (void)emulatorOpenTextureFolder:(NSString*)path;
- (void)emulatorQuit;
@end

@interface DKControllerInfo : NSObject
@property(nonatomic, readonly) NSString* name;
@property(nonatomic, readonly) NSString* identifier;
@end

/// A frame for the stereo presenter.
@interface DKStereoFrame : NSObject
/// sRGB-encoded colour.
@property(nonatomic, readonly) id<MTLTexture> texture;
/// 2 when the texture holds the left and right eye side by side, else 1.
@property(nonatomic, readonly) NSInteger eyes;
/// Immersive wide view: whether the frame carries the game camera below.
@property(nonatomic, readonly) BOOL hasCamera;
/// The game camera's camera-to-world rotation (game world, y up; the camera
/// looks down -z with y up).
@property(nonatomic, readonly) simd_quatf cameraRotation;
/// Tangents of half the image's horizontal and vertical angle.
@property(nonatomic, readonly) float tanHalfWidth;
@property(nonatomic, readonly) float tanHalfHeight;
/// The immersive view's HUD for this frame (premultiplied alpha), or nil. It
/// is in the guest framebuffer's rotated orientation: texture x runs up the
/// screen from its bottom edge, texture y leftward from its right edge.
@property(nonatomic, readonly, nullable) id<MTLTexture> hudTexture;
/// Display-oriented host UI for the immersive HUD plane.
@property(nonatomic, readonly, nullable) id<MTLTexture> hostHudTexture;
/// The head look the frame's view was turned by (DKEmulator setHeadLook).
@property(nonatomic, readonly) BOOL hasHeadLook;
@property(nonatomic, readonly) float headLookYaw;
@property(nonatomic, readonly) float headLookPitch;
/// The game drew this frame without the head look while the head was turned.
@property(nonatomic, readonly) BOOL isUnturned;
/// A full-screen menu is open (the pause pages, the Soaring map): the image
/// holds the whole screen as on a 3DS (no HUD layer, no head look), for a
/// flat panel.
@property(nonatomic, readonly) BOOL isMenu;
@end

@interface DKEmulator : NSObject

@property(class, nonatomic, readonly) DKEmulator* shared;
@property(nonatomic, weak, nullable) id<DKEmulatorHost> host;

/// Requests the debugger-prepared JIT arena. Call first thing at launch.
+ (void)initializeJit;
@property(nonatomic, readonly) BOOL jitEnabled;
/// Use the JIT when it is available (saved in the app's user defaults). Off,
/// the game runs the ahead-of-time code even with a JIT enabler attached.
/// Applies the next time the game starts or restarts.
@property(nonatomic) BOOL allowJIT;
/// Whether this build contains ahead-of-time translated game code.
@property(nonatomic, readonly) BOOL aotLinked;
/// The CPU backend of the running game: "JIT", "Ahead-of-time code" or
/// "Interpreter" (the backend the last start chose).
@property(nonatomic, readonly) NSString* cpuBackend;

// MARK: Setup

/// Checks the Dekui folder (Documents), its game and prepared user folder.
/// On success the emulator's user directory, logging and settings are ready.
/// May be called off the main thread (verifying the game reads the whole
/// dump); the main thread must not be blocked waiting for it.
- (DKSetupStatus)prepareWithMessage:(NSString* _Nullable* _Nullable)message;
@property(nonatomic, readonly) NSURL* gameFolderURL;

// MARK: Emulation

@property(nonatomic, readonly) DKEmulationState state;
- (void)startWithLayer:(CAMetalLayer*)layer size:(CGSize)size;
/// The game view's size in points; the layer draws one pixel per point.
- (void)setViewSize:(CGSize)size;
- (void)pause;
- (void)resume;
/// Reboots the game in place, keeping the window and settings.
- (void)restart;
- (void)stop;
/// Drives the frontend's periodic work. Call once per display frame.
- (void)tick;

// MARK: Overlay and input

@property(nonatomic, readonly) BOOL dekuiSettingsAvailable;
@property(nonatomic, readonly) BOOL dekuiSettingsVisible;
- (void)toggleDekuiSettings;
/// Moves the Game Options focus with a controller. Ignored while it is hidden.
- (void)overlayInput:(DKOverlayInput)input down:(BOOL)down
    NS_SWIFT_NAME(overlayInput(_:down:));
- (void)pointerDownAt:(CGPoint)point;
- (void)pointerMovedTo:(CGPoint)point;
- (void)pointerUpAt:(CGPoint)point;
- (void)scrollAt:(CGPoint)point delta:(CGVector)delta;
/// Returns NO when the key should go to the system instead.
- (BOOL)handleKey:(UIKey*)key down:(BOOL)down;
- (void)releaseAllKeys;

// MARK: Settings (applied on the next tick, saved to qt-config.ini)

/// Internal resolution multiplier, 1-10.
@property(nonatomic) NSInteger resolution;
/// 0-100.
@property(nonatomic) NSInteger volume;
@property(nonatomic) BOOL vsync;
@property(nonatomic) BOOL diskShaderCache;
@property(nonatomic) BOOL hardwareShaders;
@property(nonatomic) BOOL asyncShaders;
@property(nonatomic) BOOL customTextures;
/// 1 English, 2 French, 5 Spanish.
@property(nonatomic) NSInteger gameLanguage;
@property(nonatomic, readonly) NSInteger gameLanguageStatus;

// MARK: Stereoscopic 3D

/// Renders the game's 3D effect for both eyes (saved to qt-config.ini). While
/// it is on, frames go to acquireStereoFrame instead of the game view's layer.
@property(nonatomic) BOOL stereoscopic;
/// The 3DS depth slider, 0-100.
@property(nonatomic) NSInteger depth3D;
/// The newest frame not taken yet, or nil. Release it once the GPU has
/// finished reading it; releaseStereoFrame may be called on any thread.
- (nullable DKStereoFrame*)acquireStereoFrame;
- (void)releaseStereoFrame:(DKStereoFrame*)frame NS_SWIFT_NAME(releaseStereoFrame(_:));

// MARK: Immersive wide view (prototype)

/// The game renders a wider field of view and every frame goes to
/// acquireStereoFrame, with its camera, for a world-locked curved surface.
@property(nonatomic) BOOL wideView;
/// The vertical field of view of a standard gameplay camera, 50-120 degrees.
@property(nonatomic) NSInteger wideViewFov;
/// How far the game world tilts to keep the character at the recentre
/// height, 0 (true horizon) to 1 (character centred).
@property(nonatomic) float wideViewCentring;
/// Head look: turns the game's view in place by the head's turn since the
/// recentre (radians; yaw positive left, pitch positive up), from the head's
/// elevation at the recentre, outside first person. Any thread.
- (void)setHeadLookYaw:(float)yaw pitch:(float)pitch anchorPitch:(float)anchorPitch
               enabled:(BOOL)enabled
    NS_SWIFT_NAME(setHeadLook(yaw:pitch:anchorPitch:enabled:));
/// The game camera is in a first-person mode (gyro aiming turns it).
@property(nonatomic, readonly) BOOL firstPersonCamera;
/// Writes a line to the emulator log (user/log), for tuning on device.
- (void)logWideView:(NSString*)message;
/// Writes a frontend line to the emulator log (user/log). Any thread.
- (void)logMessage:(NSString*)message NS_SWIFT_NAME(logMessage(_:));

// MARK: Controls

@property(nonatomic, readonly) NSArray<DKControllerInfo*>* connectedControllers;
- (NSString*)bindingForControl:(DKControl)control;
/// Waits for the next button, key or stick movement and binds it.
/// `completion` receives the new binding description, or nil if cancelled.
- (void)captureBindingForControl:(DKControl)control
                      completion:(void (^)(NSString* _Nullable binding))completion;
/// Binds one direction of the Circle Pad or C-Stick to a button, key or stick
/// direction. A stick bound as a whole is converted to per-direction bindings.
- (NSString*)bindingForStick:(DKControl)stick direction:(DKStickDirection)direction;
- (void)captureBindingForStick:(DKControl)stick
                     direction:(DKStickDirection)direction
                    completion:(void (^)(NSString* _Nullable binding))completion;
- (void)cancelCapture;
/// Returns to the automatic mapping for the connected controller.
- (void)resetControlsToAutomatic;

@end

NS_ASSUME_NONNULL_END
