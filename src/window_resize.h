#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "render_scale.h"

// The render size following a resized window (Issue #365). The swap chain
// follows the window at once, every frame of a drag; the frame's own targets
// (the framebuffer and the title's render surfaces, made at the render
// scale) are made again only once the size has settled, so dragging an edge
// does not remake hundreds of megabytes of targets a frame.
namespace sfr::window_resize {

// SFR_RENDER_SIZE: the launcher's Render Size, 0 for Original or a percent
// of the window's (the display's in full screen) height, as settings.ini's
// resolution_scale. Absent, empty or not a whole number up to
// render_scale::max_percent: the render size stays what SFR_RENDER_SCALE
// made it (started without the launcher, as before).
[[nodiscard]] inline std::optional<uint32_t> parse_render_size(const char* text) noexcept {
    if (!text || !*text) return std::nullopt;
    uint32_t value = 0;
    for (const char* c = text; *c; ++c) {
        if (*c < '0' || *c > '9') return std::nullopt;
        value = value * 10 + uint32_t(*c - '0');
        if (value > render_scale::max_percent) return std::nullopt;
    }
    return value;
}

// The percent a window (or display) output_height lines tall renders at, for
// a logical_width x logical_height frame at that Render Size: the launcher's
// own (render_scale::percent_of_output), lowered as the presentation lowers it.
[[nodiscard]] inline uint32_t percent_for(uint32_t output_height, uint32_t render_size,
                                          uint32_t logical_width, uint32_t logical_height) noexcept {
    return render_scale::fit_percent(render_scale::percent_of_output(output_height, render_size),
                                     logical_width, logical_height);
}

// Waits for a window size to settle: it is answered once, when it has stayed
// the same for settle_time while the player was not dragging the window's
// edge (Windows says when a drag ends; elsewhere the time alone decides).
// A size of 0 (a minimized window) is never answered and changes nothing:
// restored to the size it had, the window needs nothing made again.
class Settle {
public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds settle_time{200};

    Settle() = default;
    Settle(uint32_t width, uint32_t height) : applied_{width, height}, seen_{width, height} {}

    // Each present: the window's size in pixels and whether it is being
    // dragged. The size to apply now, or nothing.
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> update(uint32_t width, uint32_t height, bool dragging,
                                                                      clock::time_point now) noexcept {
        if (!width || !height) return std::nullopt;
        const std::pair size{width, height};
        if (size != seen_) {
            seen_ = size;
            changed_ = now;
        }
        if (dragging || seen_ == applied_ || now - changed_ < settle_time) return std::nullopt;
        applied_ = seen_;
        return applied_;
    }
    [[nodiscard]] std::pair<uint32_t, uint32_t> applied() const noexcept { return applied_; }

private:
    std::pair<uint32_t, uint32_t> applied_{0, 0}, seen_{0, 0};
    clock::time_point changed_{};
};

// SFR_TEST_WINDOW_RESIZE (tests and verification only): the game changes
// its own window at given presents, as a player dragging it would.
// "<present>:<step>,..." where a step is <width>x<height> (the window's
// client area in pixels), min (minimized), restore, full (full screen) or
// window (out of full screen). Steps out of order or not understood are
// left out.
struct ScriptStep {
    enum class Action { size, minimize, restore, fullscreen, windowed };
    uint64_t present = 0;
    Action action = Action::size;
    uint32_t width = 0, height = 0;
};
[[nodiscard]] inline std::vector<ScriptStep> parse_script(std::string_view text) {
    std::vector<ScriptStep> steps;
    const auto number = [](std::string_view digits, uint64_t& value) {
        if (digits.empty() || digits.size() > 9) return false;
        value = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + uint64_t(c - '0');
        }
        return true;
    };
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const size_t colon = item.find(':');
        if (colon == std::string_view::npos) continue;
        ScriptStep step;
        if (!number(item.substr(0, colon), step.present)) continue;
        if (!steps.empty() && step.present < steps.back().present) continue;
        const std::string_view what = item.substr(colon + 1);
        if (what == "min") step.action = ScriptStep::Action::minimize;
        else if (what == "restore") step.action = ScriptStep::Action::restore;
        else if (what == "full") step.action = ScriptStep::Action::fullscreen;
        else if (what == "window") step.action = ScriptStep::Action::windowed;
        else {
            const size_t x = what.find('x');
            uint64_t width = 0, height = 0;
            if (x == std::string_view::npos || !number(what.substr(0, x), width) || !number(what.substr(x + 1), height) ||
                width < 160 || height < 160 || width > 16384 || height > 16384)
                continue;
            step.width = uint32_t(width);
            step.height = uint32_t(height);
        }
        steps.push_back(step);
    }
    return steps;
}

}
