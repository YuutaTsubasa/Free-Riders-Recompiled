#include "ios_screen.h"

#import <CoreText/CoreText.h>
#import <UIKit/UIKit.h>

namespace sfr::launcher {

Insets ios_safe_area() {
    @autoreleasepool {
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow* window in static_cast<UIWindowScene*>(scene).windows) {
                if (!window.isKeyWindow) continue;
                const UIEdgeInsets insets = window.safeAreaInsets;
                return {float(insets.left), float(insets.top), float(insets.right), float(insets.bottom)};
            }
        }
        return {};
    }
}

std::string ios_font_file(const char* postscript_name) {
    @autoreleasepool {
        NSString* name = [NSString stringWithUTF8String:postscript_name];
        CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithNameAndSize((__bridge CFStringRef)name, 12.0);
        if (!descriptor) return {};
        CTFontRef font = CTFontCreateWithFontDescriptor(descriptor, 12.0, nullptr);
        CFRelease(descriptor);
        if (!font) return {};
        // CoreText substitutes a font it has for one it lacks: only the asked one counts.
        NSString* found = CFBridgingRelease(CTFontCopyPostScriptName(font));
        NSURL* url = CFBridgingRelease(CTFontCopyAttribute(font, kCTFontURLAttribute));
        CFRelease(font);
        if (![found isEqualToString:name] || !url.fileURL) return {};
        return std::string(url.path.fileSystemRepresentation);
    }
}

}  // namespace sfr::launcher
