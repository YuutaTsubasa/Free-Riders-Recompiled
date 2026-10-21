#include "kinect_ideal_diagram.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close_to(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

bool inside(const sfr::KinectDiagramPoint& p, float width, float height) {
    return p.x >= 0 && p.x <= width && p.y >= 0 && p.y <= height;
}

sfr::KinectIdealSnapshot two_players() {
    sfr::KinectIdealSnapshot snapshot;
    snapshot.valid = snapshot.placed = snapshot.floor_known = true;
    // Players at 2.0 and 2.4 m, mean x 0.1: the camera 2.5 m behind their line.
    snapshot.players = {{1, 0, -0.4f, 2.0f, 2.05f}, {2, 1, 0.6f, 2.4f, 2.45f}};
    snapshot.camera_x = 0.1f;
    snapshot.camera_z = 2.2f - 2.5f;
    snapshot.distance = 2.5f;
    return snapshot;
}

void names() {
    require(std::string(sfr::kinect_depth_choice_name(sfr::KinectDepthChoiceView::ideal)) == "ideal" &&
                std::string(sfr::kinect_depth_choice_name(sfr::KinectDepthChoiceView::fit)) == "fit" &&
                std::string(sfr::kinect_depth_choice_name(sfr::KinectDepthChoiceView::sensor)) == "sensor",
            "the environment's names");
    const sfr::KinectDepthChoice choice;
    require(choice.view == sfr::KinectDepthChoiceView::ideal && choice.distance == 2.5f && choice.height == 1.0f &&
                choice.follow == 2.0f && choice.deadband == 0.15f && choice.skeleton_distance == 0,
            "the defaults are Recompiled's");
}

void everything_fits() {
    const float width = 300, height = 400;
    const auto diagram = sfr::kinect_ideal_diagram(two_players(), width, height);
    require(diagram.scale > 0, "drawn");
    require(diagram.scale * 5.0f <= width + 1e-3f, "at least 2.5 m each side");
    // The v2 at the bottom, in the middle; the virtual v1 behind it (lower).
    const auto v2 = diagram.v2_view[0], v1 = diagram.v1_view[0];
    require(close_to(v2.x, width / 2) && inside(v2, width, height) && inside(v1, width, height), "both cameras fit");
    require(v1.y > v2.y, "the virtual v1 stands behind the v2, drawn below it");
    require(close_to(v1.x - v2.x, 0.1f * diagram.scale), "laterally on the players");
    for (const auto& player : diagram.players) require(inside(player.at, width, height), "every player fits");
    require(diagram.players[0].at.x < diagram.players[1].at.x, "+x drawn to the right, as the depth images");
    require(diagram.players[0].at.y > diagram.players[1].at.y, "nearer is lower");
    require(close_to(v1.y - diagram.players_y, 2.5f * diagram.scale), "the players' line distance in front of the v1");
    // The far corners spread at each camera's half angle.
    const float v2_half = (diagram.v2_view[2].x - diagram.v2_view[1].x) / 2 / (v2.y - diagram.v2_view[1].y);
    const float v1_half = (diagram.v1_view[2].x - diagram.v1_view[1].x) / 2 / (v1.y - diagram.v1_view[1].y);
    require(close_to(v2_half, std::tan(35.3f * 3.14159265f / 180.0f), 1e-3f), "the v2's 70.6 degrees");
    require(close_to(v1_half, std::tan(28.5f * 3.14159265f / 180.0f), 1e-3f), "the v1's 57 degrees");
    require(close_to(v1.y - diagram.v1_near_y, 0.8f * diagram.scale) && close_to(v1.y - diagram.v1_far_y, 4.0f * diagram.scale),
            "the v1's usable depths");
    require(close_to(diagram.v2_view[1].y, diagram.v1_view[1].y, 1e-2f) && close_to(diagram.v2_view[1].y, 0, 1e-2f),
            "both views reach the drawing's top");
}

void numbers_and_notes() {
    const auto diagram = sfr::kinect_ideal_diagram(two_players(), 300, 400);
    const auto& one = diagram.players[0];
    const auto& two = diagram.players[1];
    require(one.slot == 0 && two.slot == 1, "1P and 2P");
    require(close_to(one.v1_depth, 2.3f) && close_to(two.v1_depth, 2.7f), "the depth the game sees");
    require(close_to(one.v2_depth, 2.05f) && close_to(two.v2_depth, 2.45f), "the v2's own");
    require(one.in_v1_view && one.in_v1_range && one.in_v2_view && two.in_v1_view && two.in_v2_view, "both seen");
    require(one.band_left < one.at.x && one.band_right > one.at.x, "inside the band of their depth");
    // At 2.3 m the v1 sees +-1.249 m about 0.1; the v2 at 2.0 m +-1.416 m.
    const float scale = diagram.scale, middle = 150;
    require(close_to(one.band_left, middle + (0.1f - 2.3f * std::tan(28.5f * 3.14159265f / 180.0f)) * scale, 0.05f) &&
                close_to(one.band_right, middle + (0.1f + 2.3f * std::tan(28.5f * 3.14159265f / 180.0f)) * scale, 0.05f),
            "the band is the v1's view there, inside the v2's");
    // Far to the side: outside the v1, still in the v2.
    auto snapshot = two_players();
    snapshot.players = {{3, 0, 1.75f, 2.6f, 2.6f}};
    auto side = sfr::kinect_ideal_diagram(snapshot, 300, 400).players[0];
    require(!side.in_v1_view && side.in_v2_view && side.in_v1_range, "outside the virtual v1's view");
    // Too far from the virtual v1.
    snapshot.players = {{3, 0, 0.1f, 3.9f, 3.9f}};
    side = sfr::kinect_ideal_diagram(snapshot, 300, 400).players[0];
    require(side.in_v1_view && !side.in_v1_range && close_to(side.v1_depth, 4.2f), "beyond the v1's 4 m");
    // Too near: the camera is placed in front of the v2 (players far away) and
    // someone steps up to it.
    snapshot.camera_z = 1.0f;
    snapshot.players = {{3, 0, 0.0f, 1.5f, 1.5f}};
    side = sfr::kinect_ideal_diagram(snapshot, 300, 400).players[0];
    require(!side.in_v1_range && close_to(side.v1_depth, 0.5f), "nearer than the v1's 0.8 m");
    snapshot.players = {{3, 0, 0.0f, 0.3f, 0.3f}};
    side = sfr::kinect_ideal_diagram(snapshot, 300, 400).players[0];
    require(!side.in_v1_view && !side.in_v2_view && side.band_left > side.band_right, "behind the camera: seen by none");
}

void grows_to_fit() {
    sfr::KinectIdealSnapshot snapshot;
    snapshot.valid = true;
    // Nobody yet: the camera at the v2.
    auto diagram = sfr::kinect_ideal_diagram(snapshot, 200, 200);
    require(diagram.scale > 0 && diagram.players.empty(), "drawn without players");
    require(close_to(diagram.v1_view[0].y, diagram.v2_view[0].y) && diagram.v2_view[0].y < 200, "above the bottom edge");
    // A player far to the side and the camera far behind.
    snapshot.camera_x = 1.0f;
    snapshot.camera_z = -2.0f;
    snapshot.players = {{1, 0, 3.4f, 3.0f, 3.0f}};
    diagram = sfr::kinect_ideal_diagram(snapshot, 400, 300);
    require(inside(diagram.v1_view[0], 400, 300) && inside(diagram.players[0].at, 400, 300), "grown to fit");
    require(sfr::kinect_ideal_diagram(snapshot, 0, 300).scale == 0, "no room: nothing");
}
}

namespace {
void rulers() {
    const float width = 320, height = 420, left = 30, bottom = 18;
    const auto diagram = sfr::kinect_ideal_diagram(two_players(), width, height, left, bottom);
    require(diagram.scale > 0, "drawn");
    // The useful range always shows, inside the strips left for the labels.
    const auto near_left = diagram.place(-2.5f, 0.5f), far_right = diagram.place(2.5f, 4.5f);
    require(near_left.x >= left - 1e-3f && far_right.x <= width + 1e-3f && far_right.y >= -1e-3f && near_left.y <= height - bottom,
            "0.5-4.5 m deep and +-2.5 m wide in view, clear of the rulers");
    require(close_to(diagram.origin_x, left + (width - left) / 2), "the v2's axis in the middle of the drawing");
    // Depth: every half metre from the v2, a whole metre labelled.
    bool one = false, two_and_a_half = false;
    for (size_t i = 0; i < diagram.depth_ticks.size(); ++i) {
        const auto& tick = diagram.depth_ticks[i];
        require(close_to(tick.at, diagram.place(0, tick.metres).y), "a depth tick at its distance from the v2");
        require(tick.whole == close_to(std::fmod(std::fabs(tick.metres), 1.0f), 0.0f), "whole metres marked");
        require(tick.at >= -1e-3f && tick.at <= height - bottom + 1e-3f, "inside the drawing");
        if (i) require(close_to(diagram.depth_ticks[i - 1].at - tick.at, 0.5f * diagram.scale, 1e-2f), "half a metre apart");
        one = one || (close_to(tick.metres, 1.0f) && tick.whole);
        two_and_a_half = two_and_a_half || (close_to(tick.metres, 2.5f) && !tick.whole);
    }
    require(one && two_and_a_half && diagram.depth_ticks.back().metres >= 4.5f, "1 m labelled, 2.5 m a half, up to 4.5 m");
    require(close_to(diagram.place(0, 1.0f).y, diagram.v2_view[0].y - diagram.scale), "1 m is a metre above the v2");
    // Across: 0 on the v2's axis, every half metre.
    bool zero = false;
    for (const auto& tick : diagram.lateral_ticks) {
        require(close_to(tick.at, diagram.origin_x + tick.metres * diagram.scale), "a lateral tick at its x");
        require(tick.at >= left - 1e-3f && tick.at <= width + 1e-3f, "inside the drawing");
        zero = zero || (tick.metres == 0 && tick.whole && close_to(tick.at, diagram.v2_view[0].x));
    }
    require(zero && diagram.lateral_ticks.front().metres <= -2.5f && diagram.lateral_ticks.back().metres >= 2.5f,
            "0 at the centre line, +-2.5 m");
    // The v1's usable depths on its own axis.
    const auto v1 = diagram.v1_view[0];
    require(close_to(diagram.v1_near_mark.x, v1.x) && close_to(diagram.v1_far_mark.x, v1.x) &&
                close_to(v1.y - diagram.v1_near_mark.y, 0.8f * diagram.scale) &&
                close_to(v1.y - diagram.v1_far_mark.y, 4.0f * diagram.scale),
            "0.8 m and 4.0 m from the virtual v1");
    require(close_to(diagram.v1_near_mark.y, diagram.v1_near_y) && close_to(diagram.v1_far_mark.y, diagram.v1_far_y),
            "on its range lines");
}
}

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"rulers", rulers},
        {"names", names},
        {"fits", everything_fits},
        {"numbers", numbers_and_notes},
        {"grows", grows_to_fit},
    };
    unsigned failures = 0;
    for (const auto& [name, test] : cases) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    if (failures) return 1;
    std::cout << "kinect_ideal_diagram: all passed\n";
    return 0;
}
