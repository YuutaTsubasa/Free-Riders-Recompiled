#include "ios_screen.h"

#import <CoreText/CoreText.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>

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

std::vector<RenderedGlyph> ios_render_glyphs(const std::vector<unsigned int>& codepoints, float pixel_size, bool bold) {
    std::vector<RenderedGlyph> glyphs;
    @autoreleasepool {
        // ImGui sizes Arial by its ascent plus descent; CoreText by the em.
        // The Chinese glyphs take Arial's em and sit on its baseline.
        float em = pixel_size * 0.9f, ascent = pixel_size * 0.8f;
        if (CTFontRef arial = CTFontCreateWithName(CFSTR("ArialMT"), 100.0, nullptr)) {
            const CGFloat up = CTFontGetAscent(arial), down = CTFontGetDescent(arial);
            if (up + down > 0) {
                em = float(pixel_size * 100.0 / (up + down));
                ascent = float(pixel_size * up / (up + down));
            }
            CFRelease(arial);
        }
        CTFontRef base = CTFontCreateWithName(bold ? CFSTR("PingFangTC-Semibold") : CFSTR("PingFangTC-Regular"), em, nullptr);
        if (!base) return glyphs;
        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        for (unsigned int codepoint : codepoints) {
            UniChar units[2];
            CFIndex count = 1;
            if (codepoint >= 0x10000) {
                const unsigned int v = codepoint - 0x10000;
                units[0] = UniChar(0xD800 + (v >> 10));
                units[1] = UniChar(0xDC00 + (v & 0x3FF));
                count = 2;
            } else {
                units[0] = UniChar(codepoint);
            }
            CFStringRef text = CFStringCreateWithCharacters(nullptr, units, count);
            if (!text) continue;
            // The face that has the character (PingFang, or CoreText's choice).
            CTFontRef font = CTFontCreateForString(base, text, CFRangeMake(0, count));
            CFRelease(text);
            if (!font) continue;
            CGGlyph pair[2] = {};
            if (!CTFontGetGlyphsForCharacters(font, units, pair, count) || !pair[0]) {
                CFRelease(font);
                continue;
            }
            const CGGlyph glyph = pair[0];
            CGRect bounds{};
            CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &bounds, 1);
            CGSize advance{};
            CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &advance, 1);
            const int left = int(std::floor(bounds.origin.x)), bottom = int(std::floor(bounds.origin.y));
            const int right = int(std::ceil(bounds.origin.x + bounds.size.width));
            const int top = int(std::ceil(bounds.origin.y + bounds.size.height));
            RenderedGlyph out;
            out.codepoint = codepoint;
            out.width = std::max(right - left + 2, 1);
            out.height = std::max(top - bottom + 2, 1);
            out.advance = float(advance.width);
            out.x = float(left - 1);
            out.y = ascent - float(top + 1);
            out.coverage.assign(size_t(out.width) * size_t(out.height), 0);
            // A bitmap context's first row is the image's top.
            CGContextRef context = CGBitmapContextCreate(out.coverage.data(), size_t(out.width), size_t(out.height), 8,
                                                         size_t(out.width), gray, kCGImageAlphaNone);
            if (context) {
                CGContextSetGrayFillColor(context, 1.0, 1.0);
                CGContextSetShouldAntialias(context, true);
                const CGPoint at = CGPointMake(1 - left, 1 - bottom);
                CTFontDrawGlyphs(font, &glyph, &at, 1, context);
                CGContextRelease(context);
                glyphs.push_back(std::move(out));
            }
            CFRelease(font);
        }
        CGColorSpaceRelease(gray);
        CFRelease(base);
    }
    return glyphs;
}

}  // namespace sfr::launcher
