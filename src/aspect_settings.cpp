#include "aspect_settings.h"
#include "native_presentation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace sfr::aspect {
namespace {

std::string lower_env(const char* name) {
    const char* text = std::getenv(name);
    std::string value = text ? text : "";
    for (char& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
// An explicit 0/1 switch: -1 when absent or empty.
int switch_env(const char* name) {
    const char* text = std::getenv(name);
    if (!text || !*text) return -1;
    return *text == '1' ? 1 : 0;
}

uint32_t size_env(const char* name) {
    const char* text = std::getenv(name);
    const unsigned long value = text ? std::strtoul(text, nullptr, 10) : 0;
    // The same bounds the presentation window takes (native_presentation.cpp).
    return value >= 160 && value <= 16384 ? uint32_t(value) : 0;
}

// The render scale the presentation will apply (SFR_RENDER_SCALE, same rules),
// so a width is never chosen that the scaled targets cannot hold (8192).
uint32_t render_percent() {
    const std::string value = lower_env("SFR_RENDER_SCALE");
    for (const char* allowed : {"50", "75", "100", "150", "200", "300"})
        if (value == allowed) return uint32_t(std::stoul(value));
    return 100;
}

uint32_t decide_width(uint32_t guest_width, uint32_t guest_height) {
    if (original_mode()) {
        std::cerr << "NATIVE_ASPECT mode=original logical_width=" << guest_width << '\n';
        return guest_width;
    }
    const uint32_t percent = render_percent();
    const uint32_t most = std::min<uint32_t>(8192, 8192u * 100u / percent) & ~1u;
    if (const char* forced = std::getenv("SFR_ASPECT_WIDTH"); forced && *forced) {
        const unsigned long value = std::strtoul(forced, nullptr, 10);
        if (value >= guest_width && value <= most) {
            std::cerr << "NATIVE_ASPECT mode=auto forced SFR_ASPECT_WIDTH logical_width=" << value
                      << " was=" << guest_width << '\n';
            return uint32_t(value);
        }
        std::cerr << "NATIVE_ASPECT invalid SFR_ASPECT_WIDTH=" << forced << " (deriving it)\n";
    }
    // A ratio the player chose: the logical width follows it, not the window.
    if (const char* ratio = std::getenv("SFR_ASPECT_RATIO"); ratio && *ratio) {
        uint32_t num = 0, den = 0;
        if (parse_ratio(ratio, num, den)) {
            const uint32_t width = width_for_ratio(guest_width, guest_height, num, den,
                                                   std::min<uint32_t>(4096, most));
            std::cerr << "NATIVE_ASPECT mode=auto ratio=" << num << ':' << den << " logical_width=" << width
                      << " was=" << guest_width << " ui_align=" << (hud_anchor() ? "edge" : "center")
                      << " menu_bg=" << menu_backdrop() << '\n';
            return width;
        }
        std::cerr << "NATIVE_ASPECT invalid SFR_ASPECT_RATIO=" << ratio << " (deriving it)\n";
    }
    // The shape the frame is shown in: the display in full screen, otherwise
    // the window the launcher asked for (the presentation makes it that size).
    uint32_t out_width = 0, out_height = 0;
    const char* source = "window";
#ifdef __ANDROID__
    const bool fullscreen = true;  // phones always fill the display
#else
    const bool fullscreen = switch_env("SFR_FULLSCREEN") == 1;
#endif
    if (fullscreen) {
        const auto [w, h] = primary_display_size();
        out_width = w; out_height = h;
        source = "display";
#ifdef __ANDROID__
        if (out_height > out_width) std::swap(out_width, out_height);  // played in landscape
#endif
    }
    if (!out_width || !out_height) {
        out_width = size_env("SFR_WINDOW_WIDTH");
        out_height = size_env("SFR_WINDOW_HEIGHT");
        source = "window";
    }
    if (!out_width || !out_height) { out_width = guest_width; out_height = guest_height; source = "default"; }
    // As wide as that shape at the title's height, even, within the bounds;
    // 16:9 or narrower (4:3, 16:10) keeps the title's width (letterboxed).
    const double exact = double(guest_height) * double(out_width) / double(out_height);
    uint32_t width = uint32_t(std::lround(exact / 2.0)) * 2u;
    width = std::clamp<uint32_t>(width, guest_width, std::min<uint32_t>(4096, most));
    if (!wider_than_16x9(width, guest_height)) width = guest_width;
    std::cerr << "NATIVE_ASPECT mode=auto source=" << source << ' ' << out_width << 'x' << out_height
              << " logical_width=" << width << " was=" << guest_width << " ui_align="
              << (hud_anchor() ? "edge" : "center") << " menu_bg=" << menu_backdrop() << '\n';
    return width;
}

}

bool original_mode() {
    static const bool original = lower_env("SFR_ASPECT_MODE") == "original";
    return original;
}

bool hud_anchor() {
    static const bool on = [] {
        if (original_mode()) return false;
        if (const int explicit_switch = switch_env("SFR_HUD_ANCHOR"); explicit_switch >= 0) return explicit_switch == 1;
        return lower_env("SFR_UI_ALIGN") != "center";
    }();
    return on;
}

bool ui_center() {
    static const bool on = [] {
        if (original_mode()) return false;
        const char* text = std::getenv("SFR_UI_CENTER");
        return !(text && *text == '0');
    }();
    return on;
}

bool menu_backdrop() {
    static const bool on = [] {
        if (original_mode()) return false;
        const int explicit_switch = switch_env("SFR_MENU_BG");
        return explicit_switch < 0 || explicit_switch == 1;
    }();
    return on;
}

uint32_t logical_width(uint32_t guest_width, uint32_t guest_height) {
    static std::mutex mutex;
    static uint32_t asked_width = 0, asked_height = 0, answer = 0;
    std::lock_guard<std::mutex> lock(mutex);
    if (!answer || asked_width != guest_width || asked_height != guest_height) {
        asked_width = guest_width; asked_height = guest_height;
        answer = decide_width(guest_width, guest_height);
    }
    return answer;
}

}
