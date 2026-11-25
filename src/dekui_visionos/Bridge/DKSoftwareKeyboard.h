// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The 3DS software keyboard (used, for example, to name a new save file),
// shown as a SwiftUI prompt. The game waits while the prompt is open.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface DKKeyboardRequest : NSObject
@property(nonatomic, readonly) NSString* hint;
@property(nonatomic, readonly) NSInteger maxLength;
@property(nonatomic, readonly) BOOL multiline;
/// Button titles, left to right; the last one confirms.
@property(nonatomic, readonly) NSArray<NSString*>* buttons;

/// Nil when the game accepts `text` with the button at `index`, otherwise the
/// reason it does not.
- (nullable NSString*)validationErrorForText:(NSString*)text button:(NSInteger)index;

/// Returns the text to the game. Call once, on the main thread.
- (void)finishWithText:(NSString*)text button:(NSInteger)index;
@end

@interface DKSoftwareKeyboard : NSObject
/// Called on the main thread when the game asks for text. The presenter must
/// eventually call -finishWithText:button: on the request.
@property(class, nonatomic, copy, nullable) void (^presenter)(DKKeyboardRequest* request);
/// Called on the main thread when the game rejects the entered text.
@property(class, nonatomic, copy, nullable) void (^errorPresenter)(NSString* message);
@end

NS_ASSUME_NONNULL_END
