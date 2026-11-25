// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import "dekui_visionos/Bridge/DKSoftwareKeyboard.h"

#include <memory>
#include <string>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/frontend/applets/swkbd.h"
#include "dekui_visionos/Bridge/software_keyboard.h"

namespace {

NSString* ToNS(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

NSString* ValidationMessage(Frontend::ValidationError error, u16 max_length) {
    using Frontend::ValidationError;
    switch (error) {
    case ValidationError::None:
        return nil;
    case ValidationError::FixedLengthRequired:
        return ToNS(fmt::format("Enter exactly {} characters.", max_length));
    case ValidationError::MaxLengthExceeded:
        return ToNS(fmt::format("Use no more than {} characters.", max_length));
    case ValidationError::BlankInputNotAllowed:
        return @"Enter something other than spaces.";
    case ValidationError::EmptyInputNotAllowed:
        return @"Enter a name.";
    case ValidationError::MaxDigitsExceeded:
        return @"Too many digits.";
    case ValidationError::AtSignNotAllowed:
        return @"The @ sign is not allowed.";
    case ValidationError::PercentNotAllowed:
        return @"The % sign is not allowed.";
    case ValidationError::BackslashNotAllowed:
        return @"The \\ sign is not allowed.";
    default:
        return @"The game does not accept this text.";
    }
}

} // namespace

@interface DKKeyboardRequest ()
- (instancetype)initWithKeyboard:(Frontend::SoftwareKeyboard*)keyboard
                          config:(const Frontend::KeyboardConfig&)config;
@property(nonatomic, readonly) std::string resultText;
@property(nonatomic, readonly) u8 resultButton;
- (void)wait;
@end

@implementation DKKeyboardRequest {
    Frontend::SoftwareKeyboard* _keyboard;
    u16 _maxLength;
    NSString* _hint;
    BOOL _multiline;
    NSArray<NSString*>* _buttons;
    dispatch_semaphore_t _done;
    std::string _resultText;
    u8 _resultButton;
    BOOL _finished;
}

- (instancetype)initWithKeyboard:(Frontend::SoftwareKeyboard*)keyboard
                          config:(const Frontend::KeyboardConfig&)config {
    if ((self = [super init])) {
        _keyboard = keyboard;
        _maxLength = config.max_text_length;
        _hint = ToNS(config.hint_text);
        _multiline = config.multiline_mode;
        _done = dispatch_semaphore_create(0);

        // The game's own button titles, or the system ones for its layout.
        NSMutableArray<NSString*>* buttons = [NSMutableArray array];
        size_t count = 1;
        switch (config.button_config) {
        case Frontend::ButtonConfig::Dual:
            count = 2;
            break;
        case Frontend::ButtonConfig::Triple:
            count = 3;
            break;
        default:
            count = 1;
            break;
        }
        static const char* defaults[3][3] = {
            {Frontend::SWKBD_BUTTON_OKAY, "", ""},
            {Frontend::SWKBD_BUTTON_CANCEL, Frontend::SWKBD_BUTTON_OKAY, ""},
            {Frontend::SWKBD_BUTTON_CANCEL, Frontend::SWKBD_BUTTON_FORGOT,
             Frontend::SWKBD_BUTTON_OKAY},
        };
        for (size_t i = 0; i < count; ++i) {
            const bool custom = i < config.button_text.size() && !config.button_text[i].empty();
            [buttons addObject:ToNS(custom ? config.button_text[i] : defaults[count - 1][i])];
        }
        _buttons = buttons;
        _resultButton = static_cast<u8>(count - 1);
    }
    return self;
}

- (NSString*)hint {
    return _hint;
}
- (NSInteger)maxLength {
    return _maxLength;
}
- (BOOL)multiline {
    return _multiline;
}
- (NSArray<NSString*>*)buttons {
    return _buttons;
}
- (std::string)resultText {
    return _resultText;
}
- (u8)resultButton {
    return _resultButton;
}

- (nullable NSString*)validationErrorForText:(NSString*)text button:(NSInteger)index {
    // Only the confirming button submits text; the others always pass.
    if (index != NSInteger(_buttons.count) - 1) {
        return nil;
    }
    return ValidationMessage(_keyboard->ValidateInput(text.UTF8String ?: ""), _maxLength);
}

- (void)finishWithText:(NSString*)text button:(NSInteger)index {
    if (_finished) {
        return;
    }
    _finished = YES;
    _resultText = text.UTF8String ?: "";
    _resultButton = static_cast<u8>(std::max<NSInteger>(0, index));
    dispatch_semaphore_signal(_done);
}

- (void)wait {
    dispatch_semaphore_wait(_done, DISPATCH_TIME_FOREVER);
}

@end

@implementation DKSoftwareKeyboard

static void (^g_presenter)(DKKeyboardRequest*);
static void (^g_error_presenter)(NSString*);

+ (void (^)(DKKeyboardRequest*))presenter {
    return g_presenter;
}
+ (void)setPresenter:(void (^)(DKKeyboardRequest*))presenter {
    g_presenter = [presenter copy];
}
+ (void (^)(NSString*))errorPresenter {
    return g_error_presenter;
}
+ (void)setErrorPresenter:(void (^)(NSString*))presenter {
    g_error_presenter = [presenter copy];
}

@end

namespace {

/// Shows the prompt and waits on the emulation thread, like the Qt dialog.
class VisionKeyboard final : public Frontend::SoftwareKeyboard {
public:
    void Execute(const Frontend::KeyboardConfig& config_) override {
        SoftwareKeyboard::Execute(config_);
        DKKeyboardRequest* request = [[DKKeyboardRequest alloc] initWithKeyboard:this
                                                                          config:config];
        void (^presenter)(DKKeyboardRequest*) = DKSoftwareKeyboard.presenter;
        if (!presenter) {
            LOG_WARNING(Frontend, "No keyboard presenter; using an empty name");
            Finalize("", request.resultButton);
            return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
          presenter(request);
        });
        [request wait];
        LOG_INFO(Frontend, "Software keyboard finished, button={}", request.resultButton);
        Finalize(request.resultText, request.resultButton);
    }

    void ShowError(const std::string& error) override {
        NSString* message = ToNS(error);
        dispatch_async(dispatch_get_main_queue(), ^{
          if (auto presenter = DKSoftwareKeyboard.errorPresenter) {
              presenter(message);
          }
        });
    }
};

} // namespace

std::shared_ptr<Frontend::SoftwareKeyboard> DekuiMakeSoftwareKeyboard() {
    return std::make_shared<VisionKeyboard>();
}
