#include "kinect_v2_depth.h"
#include "kinect_v2_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace sfr {
namespace {
float number(const char* text, float fallback, float low, float high) {
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || !std::isfinite(value)) return fallback;
    return std::clamp(value, low, high);
}

bool none(const std::array<float, 3>& p) { return p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f; }

bool known_floor(const std::array<float, 4>& floor) { return kinect_floor_known(floor); }

std::array<float, 3> turn(const std::array<float, 9>& r, const std::array<float, 3>& p) {
    return {r[0] * p[0] + r[1] * p[1] + r[2] * p[2], r[3] * p[0] + r[4] * p[1] + r[5] * p[2],
            r[6] * p[0] + r[7] * p[1] + r[8] * p[2]};
}
// The transpose: the turn undone.
std::array<float, 3> unturn(const std::array<float, 9>& r, const std::array<float, 3>& p) {
    return {r[0] * p[0] + r[3] * p[1] + r[6] * p[2], r[1] * p[0] + r[4] * p[1] + r[7] * p[2],
            r[2] * p[0] + r[5] * p[1] + r[8] * p[2]};
}
}

bool kinect_floor_known(const std::array<float, 4>& floor) {
    if (!std::isfinite(floor[0]) || !std::isfinite(floor[1]) || !std::isfinite(floor[2]) || !std::isfinite(floor[3]))
        return false;
    const float length = std::sqrt(floor[0] * floor[0] + floor[1] * floor[1] + floor[2] * floor[2]);
    return std::fabs(length - 1.0f) < 0.05f && floor[1] > 0.5f;
}

bool kinect_floor_height_known(const std::array<float, 4>& floor) {
    return kinect_floor_known(floor) && std::isfinite(floor[3]) && floor[3] >= kinect_floor_height_minimum;
}

std::array<float, 9> kinect_level_rotation(const std::array<float, 4>& floor) {
    if (!kinect_floor_known(floor)) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double length = std::sqrt(double(floor[0]) * floor[0] + double(floor[1]) * floor[1] + double(floor[2]) * floor[2]);
    const double nx = floor[0] / length, ny = floor[1] / length, nz = floor[2] / length;
    // Roll about z: (nx, ny) -> (0, h).
    const double h = std::sqrt(nx * nx + ny * ny);
    const double c = ny / h, s = nx / h;
    // Then pitch about x: (h, nz) -> (1, 0).
    const double c2 = h, s2 = -nz;
    // R = Rx * Rz, Rz = [c -s 0; s c 0; 0 0 1], Rx = [1 0 0; 0 c2 -s2; 0 s2 c2].
    return {float(c), float(-s), 0.0f,
            float(c2 * s), float(c2 * c), float(-s2),
            float(s2 * s), float(s2 * c), float(c2)};
}

void kinect_floor_tilt(const std::array<float, 4>& floor, float& pitch_degrees, float& roll_degrees) {
    pitch_degrees = roll_degrees = 0;
    if (!kinect_floor_known(floor)) return;
    constexpr double degrees = 180.0 / 3.14159265358979323846;
    // Looking down, the up direction leans back towards the camera: -z.
    pitch_degrees = float(std::atan2(-double(floor[2]), std::hypot(double(floor[0]), double(floor[1]))) * degrees);
    roll_degrees = float(std::atan2(double(floor[0]), double(floor[1])) * degrees);
}

uint16_t kinect_v1_depth_pixel(uint32_t millimetres, uint32_t player) {
    if (millimetres < kinect_v1_depth_minimum_mm || millimetres > kinect_v1_depth_maximum_mm)
        return kinect_v1_depth_no_value;
    return uint16_t(millimetres << kinect_v1_player_index_shift | (player & 7u));
}

bool kinect_v1_depth_project(const std::array<float, 3>& point, float& x, float& y) {
    // NuiTransformSkeletonToDepthImage's own test (z > FLT_EPSILON).
    if (!(point[2] > std::numeric_limits<float>::epsilon())) return false;
    x = kinect_v1_depth_centre_x + point[0] * kinect_v1_depth_focal / point[2];
    y = kinect_v1_depth_centre_y - point[1] * kinect_v1_depth_focal / point[2];
    return std::isfinite(x) && std::isfinite(y);
}

std::array<float, 3> kinect_depth_view_point(const KinectDepthView& view, const std::array<float, 3>& given) {
    const std::array<float, 3> p = view.rotated ? turn(view.rotation, given) : given;
    return {p[0] - view.origin[0], p[1] - view.origin[1], (p[2] - view.origin[2]) * view.z_scale};
}

std::array<float, 3> kinect_depth_view_unpoint(const KinectDepthView& view, const std::array<float, 3>& p) {
    const std::array<float, 3> moved{p[0] + view.origin[0], p[1] + view.origin[1], p[2] / view.z_scale + view.origin[2]};
    return view.rotated ? unturn(view.rotation, moved) : moved;
}

void kinect_depth_view_frame(const KinectDepthView& view, KinectFrame& frame) {
    const auto move = [&](std::array<float, 3>& p) {
        if (!none(p)) p = kinect_depth_view_point(view, p);
    };
    for (KinectBody& body : frame.bodies) {
        move(body.position);
        for (auto& joint : body.joints) move(joint);
        for (auto& tip : body.hand_tips) move(tip);
        for (auto& thumb : body.thumbs) move(thumb);
    }
    // A direction turns with the camera (and is not moved).
    if (view.rotated && !none(frame.gravity)) frame.gravity = turn(view.rotation, frame.gravity);
    // n.p + d = 0 with p = R^T ((x', y', z' / k) + origin):
    // (R n).((x', y', z' / k) + origin) + d = 0, so with m = R n,
    // (mx, my, mz / k).p' + (d + m.origin) = 0, renormalised.
    auto& floor = frame.floor_plane;
    if (floor[0] == 0.0f && floor[1] == 0.0f && floor[2] == 0.0f) return;
    if (view.rotated) {
        const std::array<float, 3> normal = turn(view.rotation, {floor[0], floor[1], floor[2]});
        floor = {normal[0], normal[1], normal[2], floor[3]};
    }
    floor[3] += floor[0] * view.origin[0] + floor[1] * view.origin[1] + floor[2] * view.origin[2];
    if (view.z_scale == 1.0f) return;
    floor[2] /= view.z_scale;
    const float length = std::sqrt(floor[0] * floor[0] + floor[1] * floor[1] + floor[2] * floor[2]);
    if (!(length > 0.0f)) return;
    for (float& value : floor) value /= length;
    // A v2's gravity is its floor's normal (no accelerometer of its own).
    frame.gravity = {floor[0], floor[1], floor[2]};
}

float kinect_v2_fit_scale(float v2_focal_length) {
    if (!(v2_focal_length > 0.0f) || !std::isfinite(v2_focal_length)) v2_focal_length = kinect_v2_typical_depth_focal;
    return kinect_v1_depth_focal / (v2_focal_length * float(kinect_v1_depth_height) / 424.0f);
}

float kinect_depth_true_minimum(float z_scale) { return float(kinect_v1_depth_minimum_mm) / 1000.0f / z_scale; }
float kinect_depth_true_maximum(float z_scale) { return float(kinect_v1_depth_maximum_mm) / 1000.0f / z_scale; }

KinectDepthSettings KinectDepthSettings::parse(const char* view, const char* distance, const char* height,
                                               const char* fit_scale, const char* follow, const char* deadband,
                                               const char* skeleton_stretch, const char* skeleton_distance) {
    KinectDepthSettings settings;
    const std::string_view name = view ? view : "";
    settings.mode = name == "sensor" ? KinectDepthViewMode::sensor
                  : name == "player" ? KinectDepthViewMode::player
                  : name == "ideal"  ? KinectDepthViewMode::ideal
                                     : KinectDepthViewMode::fit;
    settings.player_view = settings.mode == KinectDepthViewMode::player;
    settings.distance = number(distance, 2.5f, 1.0f, 4.0f);
    settings.height = number(height, settings.mode == KinectDepthViewMode::ideal ? 1.0f : 0.8f, 0.2f, 2.0f);
    settings.fit_scale = number(fit_scale, 0.0f, 1.0f, 2.0f);
    settings.follow = number(follow, 2.0f, 0.2f, 20.0f);
    settings.deadband = number(deadband, 0.15f, 0.0f, 1.0f);
    // Real metres unless asked otherwise; "depth" follows the depth's k.
    settings.skeleton_stretch = skeleton_stretch && std::string_view(skeleton_stretch) == "depth"
                                    ? 0.0f
                                    : number(skeleton_stretch, 1.0f, 1.0f, 2.0f);
    settings.skeleton_distance = number(skeleton_distance, 0.0f, 1.0f, 4.0f);
    return settings;
}

const char* KinectDepthSettings::view_name() const {
    switch (mode) {
    case KinectDepthViewMode::sensor: return "sensor";
    case KinectDepthViewMode::player: return "player";
    case KinectDepthViewMode::ideal: return "ideal";
    case KinectDepthViewMode::fit: break;
    }
    return "fit";
}

KinectDepthSettings kinect_depth_settings(const KinectDepthChoice& choice) {
    const auto text = [](float value) {
        char out[32];
        std::snprintf(out, sizeof out, "%.2f", double(value));
        return std::string(out);
    };
    const std::string distance = text(choice.distance), height = text(choice.height), follow = text(choice.follow),
                      deadband = text(choice.deadband), skeleton = text(choice.skeleton_distance);
    // The ideal view's height default is 1.0 and fit's 0.8; the tuning is
    // Recompiled's, so it is read as ideal's and the view set afterwards.
    KinectDepthSettings settings = KinectDepthSettings::parse("ideal", distance.c_str(), height.c_str(), nullptr,
                                                              follow.c_str(), deadband.c_str(), nullptr,
                                                              choice.skeleton_distance > 0 ? skeleton.c_str() : nullptr);
    const KinectDepthSettings view = KinectDepthSettings::parse(kinect_depth_choice_name(choice.view), nullptr, nullptr);
    settings.mode = view.mode;
    settings.player_view = view.player_view;
    return settings;
}

KinectDepthSettings KinectDepthSettings::from_environment() {
    return parse(std::getenv("SFR_KINECT_V2_DEPTH_VIEW"), std::getenv("SFR_KINECT_V2_DEPTH_DISTANCE"),
                 std::getenv("SFR_KINECT_V2_DEPTH_HEIGHT"), std::getenv("SFR_KINECT_V2_DEPTH_FIT_SCALE"),
                 std::getenv("SFR_KINECT_V2_DEPTH_FOLLOW"), std::getenv("SFR_KINECT_V2_DEPTH_DEADBAND"),
                 std::getenv("SFR_KINECT_V2_SKELETON_STRETCH"), std::getenv("SFR_KINECT_V2_SKELETON_DISTANCE"));
}
KinectDepthView KinectDepthViewLatch::place(const std::array<float, 3>& reference,
                                            const std::array<float, 4>& floor) const {
    KinectDepthView view;
    if (!settings_.player_view) return view;
    // Along the sensor's z, so the reference is distance away.
    view.origin[2] = reference[2] - settings_.distance;
    // And along its y so that the camera is height above the floor as seen
    // from the reference point: the reference's height over the floor less
    // the camera's is where it sits in the image, as for a level v1.
    if (known_floor(floor)) {
        const float above = floor[0] * reference[0] + floor[1] * reference[1] + floor[2] * reference[2] + floor[3];
        view.origin[1] = reference[1] - (above - settings_.height);
    }
    return view;
}

const KinectDepthView& KinectDepthViewLatch::update(const KinectFrame& frame, double seconds) {
    if (!settings_.player_view) {
        view_ = {};
        return view_;
    }
    const bool anyone = !frame.bodies.empty();
    if (anyone) last_seen_ = seconds;
    if (state_ != State::empty && !anyone && seconds - last_seen_ >= release_seconds) {
        // Nobody for a while: the next person places it again. The view
        // stays meanwhile (there is nobody to place).
        state_ = State::empty;
        count_ = 0;
    }
    if (!anyone || state_ == State::latched) return view_;
    const KinectBody* body = nullptr;
    if (state_ == State::settling)
        for (const KinectBody& candidate : frame.bodies)
            if (candidate.tracking_id == tracking_id_) body = &candidate;
    if (!body) {
        // Someone new (or the one settling left): the most central body.
        body = &*std::min_element(frame.bodies.begin(), frame.bodies.end(), [](const KinectBody& a, const KinectBody& b) {
            return std::fabs(a.position[0]) < std::fabs(b.position[0]);
        });
        state_ = State::settling;
        tracking_id_ = body->tracking_id;
        count_ = 0;
        sum_ = {};
        view_ = place(body->position, frame.floor_plane);
    }
    for (int i = 0; i < 3; ++i) sum_[i] += body->position[i];
    if (++count_ >= settle_frames) {
        const std::array<float, 3> mean{float(sum_[0] / count_), float(sum_[1] / count_), float(sum_[2] / count_)};
        view_ = place(mean, frame.floor_plane);
        state_ = State::latched;
    }
    return view_;
}

KinectIdealState kinect_ideal_step(const KinectIdealState& previous, const KinectDepthSettings& settings, double seconds,
                                   const KinectFrame& bodies) {
    KinectIdealState state = previous;
    const double dt = previous.started ? std::max(0.0, seconds - previous.seconds) : 0.0;
    state.started = true;
    state.seconds = seconds;
    // The floor: the first known one as it is, then smoothed; an unknown
    // one (a v2 sends zeros while it has none) leaves it.
    // The normal and the distance apart: a v2 knows the normal (gravity)
    // long before the distance, which it sends as 0 meanwhile; smoothing that
    // 0 in would put the camera height metres above the floor.
    bool newly_level = false;
    if (kinect_floor_known(bodies.floor_plane)) {
        const bool has_height = kinect_floor_height_known(bodies.floor_plane);
        if (!state.floor_known) {
            state.floor = bodies.floor_plane;
            state.floor_known = newly_level = true;
            state.height_known = has_height;
            if (!has_height) state.floor[3] = 0;
        } else {
            const float a = float(1.0 - std::exp(-dt / kinect_ideal_floor_seconds));
            for (int i = 0; i < 3; ++i) state.floor[i] += a * (bodies.floor_plane[i] - state.floor[i]);
            const float length =
                std::sqrt(state.floor[0] * state.floor[0] + state.floor[1] * state.floor[1] + state.floor[2] * state.floor[2]);
            if (length > 0.0f)
                for (int i = 0; i < 3; ++i) state.floor[i] /= length;
            if (has_height && !state.height_known) {
                state.floor[3] = bodies.floor_plane[3];
                state.height_known = true;
            } else if (has_height) {
                state.floor[3] += a * (bodies.floor_plane[3] - state.floor[3]);
            }
        }
    }
    // The players' mean SpineBase in the levelled space.
    const std::array<float, 9> level = kinect_level_rotation(state.floor_known ? state.floor : std::array<float, 4>{});
    double sum_x = 0, sum_z = 0;
    uint32_t count = 0;
    for (const KinectBody& body : bodies.bodies) {
        const auto& p = body.position;
        if (none(p) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        const std::array<float, 3> q = state.floor_known ? turn(level, p) : p;
        sum_x += q[0];
        sum_z += q[2];
        ++count;
    }
    state.bodies = count;
    if (!count) {
        // Nobody: the camera holds; the next body is taken at once.
        state.had_bodies = false;
        return state;
    }
    const std::array<float, 2> measured{float(sum_x / count), float(sum_z / count)};
    if (!state.placed || !previous.had_bodies || newly_level) {
        state.reference = measured;
        state.placed = true;
    } else {
        const float a = float(1.0 - std::exp(-dt / double(settings.follow)));
        for (int i = 0; i < 2; ++i) {
            const float error = measured[i] - state.reference[i];
            if (std::fabs(error) <= settings.deadband) continue;
            state.reference[i] += a * (error - std::copysign(settings.deadband, error));
        }
    }
    state.had_bodies = true;
    return state;
}

KinectDepthView kinect_ideal_view(const KinectIdealState& state, const KinectDepthSettings& settings) {
    KinectDepthView view;
    // Nobody yet: straight in front of the v2's own place.
    const std::array<float, 2> reference = state.placed ? state.reference : std::array<float, 2>{0.0f, settings.distance};
    view.origin[0] = reference[0];
    view.origin[2] = reference[1] - settings.distance;
    if (state.floor_known) {
        view.rotated = true;
        view.rotation = kinect_level_rotation(state.floor);
        // Levelled, the floor is y = -d: the camera height above it. Without
        // a distance yet, the sensor's own height (y = 0).
        if (state.height_known) view.origin[1] = -state.floor[3] + settings.height;
    }
    return view;
}

KinectIdealSnapshot kinect_ideal_snapshot(const KinectFrame& bodies, const KinectIdealState& state,
                                          const KinectDepthSettings& settings) {
    KinectIdealSnapshot snapshot;
    snapshot.valid = true;
    snapshot.floor_known = state.floor_known;
    snapshot.placed = state.placed;
    const KinectDepthView view = kinect_ideal_view(state, settings);
    snapshot.camera_x = view.origin[0];
    snapshot.camera_z = view.origin[2];
    snapshot.distance = settings.distance;
    const std::array<float, 9> level = kinect_level_rotation(state.floor_known ? state.floor : std::array<float, 4>{});
    for (const KinectBody& body : bodies.bodies) {
        const auto& p = body.position;
        if (none(p) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        const std::array<float, 3> q = state.floor_known ? turn(level, p) : p;
        KinectIdealPlayer& player = snapshot.players.emplace_back();
        player.tracking_id = body.tracking_id;
        player.x = q[0];
        player.z = q[2];
        player.sensor_z = p[2];
    }
    return snapshot;
}

KinectDepthViewer::KinectDepthViewer(const KinectDepthSettings& settings) : settings_(settings), latch_(settings) {
    fit_scale_ = settings.fit_scale > 0 ? settings.fit_scale : kinect_v2_fit_scale(kinect_v2_typical_depth_focal);
    if (settings_.mode == KinectDepthViewMode::fit) view_.z_scale = fit_scale_;
    if (settings_.mode == KinectDepthViewMode::ideal) view_ = kinect_ideal_view(ideal_, settings_);
}

void KinectDepthViewer::set_settings(const KinectDepthSettings& settings) {
    const bool same_latch = settings.mode == settings_.mode && settings.distance == settings_.distance &&
                            settings.height == settings_.height;
    settings_ = settings;
    if (!same_latch) latch_ = KinectDepthViewLatch(settings_);
    if (settings_.fit_scale > 0) fit_scale_ = settings_.fit_scale;
    switch (settings_.mode) {
    case KinectDepthViewMode::sensor: view_ = {}; break;
    case KinectDepthViewMode::fit: view_ = {}; view_.z_scale = fit_scale_; break;
    case KinectDepthViewMode::player: view_ = latch_.view(); break;
    case KinectDepthViewMode::ideal: view_ = kinect_ideal_view(ideal_, settings_); break;
    }
}

void KinectDepthViewer::set_fit_scale(float z_scale) {
    fit_scale_ = z_scale;
    if (settings_.mode == KinectDepthViewMode::fit) view_.z_scale = fit_scale_;
}

const KinectDepthView& KinectDepthViewer::body_frame(const KinectFrame& frame, double seconds) {
    bodies_.floor_plane = frame.floor_plane;
    bodies_.gravity = frame.gravity;
    bodies_.bodies.clear();
    for (const KinectBody& body : frame.bodies) {
        KinectBody& kept = bodies_.bodies.emplace_back();
        kept.tracking_id = body.tracking_id;
        kept.sensor_index = body.sensor_index;
        kept.position = body.position;
        kept.joint_states[0] = body.joint_states[0];
    }
    bodies_seconds_ = seconds;
    switch (settings_.mode) {
    case KinectDepthViewMode::player: view_ = latch_.update(frame, seconds); break;
    case KinectDepthViewMode::sensor: view_ = {}; break;
    case KinectDepthViewMode::fit: view_ = {}; view_.z_scale = fit_scale_; break;
    case KinectDepthViewMode::ideal: break;  // stepped with the depth
    }
    return view_;
}

KinectDepthView KinectDepthViewer::skeleton_view() const {
    KinectDepthView view = view_;
    if (settings_.mode == KinectDepthViewMode::fit && settings_.skeleton_stretch > 0)
        view.z_scale = settings_.skeleton_stretch;
    if (settings_.mode == KinectDepthViewMode::ideal && settings_.skeleton_distance > 0)
        view.origin[2] += settings_.distance - settings_.skeleton_distance;
    return view;
}

const KinectDepthView& KinectDepthViewer::depth_frame(double seconds) {
    // Stepped in every view (the preview's diagram); only ideal is made through it.
    ideal_ = kinect_ideal_step(ideal_, settings_, seconds, bodies_);
    if (settings_.mode == KinectDepthViewMode::ideal) view_ = kinect_ideal_view(ideal_, settings_);
    return view_;
}

KinectDepthStats kinect_v2_synthesize_depth(const float* points, const uint8_t* body_index, size_t count,
                                            const KinectDepthView& view, KinectImage& out) {
    constexpr uint32_t width = kinect_v1_depth_width, height = kinect_v1_depth_height;
    constexpr size_t pixels = size_t(width) * height;
    constexpr float empty = std::numeric_limits<float>::infinity();
    std::vector<float> nearest(pixels, empty);
    std::vector<uint8_t> player(pixels, 0);
    for (size_t i = 0; i < count; ++i) {
        const float* p = points + 3 * i;
        if (!(p[2] > 0.0f) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        const std::array<float, 3> seen = kinect_depth_view_point(view, {p[0], p[1], p[2]});
        float x = 0, y = 0;
        if (!kinect_v1_depth_project(seen, x, y)) continue;
        // Pixel n is at n (NuiTransformDepthImageToSkeleton's (x - 160)).
        const float column = std::floor(x + 0.5f), row = std::floor(y + 0.5f);
        if (column < 0 || row < 0 || column >= float(width) || row >= float(height)) continue;
        const size_t at = size_t(row) * width + size_t(column);
        if (seen[2] < nearest[at]) {
            nearest[at] = seen[2];
            const uint32_t body = body_index ? body_index[i] : kinect_v2_no_body;
            player[at] = uint8_t(body < 6 ? body + 1 : 0);
        }
    }
    KinectDepthStats stats;
    // Holes one pixel wide (the two cameras' pixels do not line up one to
    // one), from the image before any is filled, only where both sides agree.
    std::vector<float> depth = nearest;
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t column = 0; column < width; ++column) {
            const size_t at = size_t(row) * width + column;
            if (nearest[at] != empty) continue;
            const auto pair = [&](size_t a, size_t b) {
                if (nearest[a] == empty || nearest[b] == empty || player[a] != player[b]) return false;
                if (std::fabs(nearest[a] - nearest[b]) > 0.05f) return false;
                depth[at] = (nearest[a] + nearest[b]) / 2.0f;
                player[at] = player[a];
                return true;
            };
            if ((column > 0 && column + 1 < width && pair(at - 1, at + 1)) ||
                (row > 0 && row + 1 < height && pair(at - width, at + width)))
                ++stats.filled;
        }
    out.width = width;
    out.height = height;
    out.bytes_per_pixel = 2;
    out.pixels.assign(pixels * 2, 0);
    for (size_t at = 0; at < pixels; ++at) {
        if (depth[at] == empty) continue;
        const uint32_t millimetres = uint32_t(std::lround(double(depth[at]) * 1000.0));
        const uint16_t value = kinect_v1_depth_pixel(millimetres, player[at]);
        out.pixels[2 * at] = uint8_t(value);
        out.pixels[2 * at + 1] = uint8_t(value >> 8);
        if (value != kinect_v1_depth_no_value) {
            ++stats.valid;
            if (value & 7u) ++stats.players;
        }
    }
    return stats;
}

KinectDepthView kinect_image_view(const KinectImage& image) {
    KinectDepthView view;
    view.origin = image.view_origin;
    view.z_scale = image.view_z_scale > 0 ? image.view_z_scale : 1.0f;
    view.rotated = image.view_rotated;
    view.rotation = image.view_rotation;
    return view;
}

void kinect_v1_depth_points(const KinectImage& depth, const KinectDepthView& view, std::vector<float>& points) {
    const size_t pixels = size_t(depth.width) * depth.height;
    points.assign(pixels * 3, 0.0f);
    if (depth.bytes_per_pixel != 2 || depth.pixels.size() < pixels * 2) return;
    for (uint32_t row = 0; row < depth.height; ++row)
        for (uint32_t column = 0; column < depth.width; ++column) {
            const size_t at = size_t(row) * depth.width + column;
            const uint32_t millimetres = uint32_t(depth.pixels[2 * at] | depth.pixels[2 * at + 1] << 8) >> kinect_v1_player_index_shift;
            if (!millimetres) continue;
            const float z = float(millimetres) / 1000.0f;
            const std::array<float, 3> seen{(float(column) - kinect_v1_depth_centre_x) * z / kinect_v1_depth_focal,
                                            -(float(row) - kinect_v1_depth_centre_y) * z / kinect_v1_depth_focal, z};
            const std::array<float, 3> p = kinect_depth_view_unpoint(view, seen);
            points[3 * at] = p[0];
            points[3 * at + 1] = p[1];
            points[3 * at + 2] = p[2];
        }
}

void kinect_v2_depth_points_from_lens(const uint16_t* millimetres, uint32_t width, uint32_t height,
                                      std::vector<float>& points) {
    points.assign(size_t(width) * height * 3, 0.0f);
    constexpr float focal = 366.0f, centre_x = 256.0f, centre_y = 212.0f;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const size_t at = size_t(y) * width + x;
            const float z = float(millimetres[at]) / 1000.0f;
            if (!(z > 0.0f)) continue;
            points[3 * at] = (float(x) - centre_x) * z / focal;
            points[3 * at + 1] = -(float(y) - centre_y) * z / focal;
            points[3 * at + 2] = z;
        }
}

void kinect_v2_depth_points_from_rays(const uint16_t* millimetres, const float* rays, size_t count,
                                      std::vector<float>& points) {
    points.assign(count * 3, 0.0f);
    for (size_t at = 0; at < count; ++at) {
        const float z = float(millimetres[at]) / 1000.0f;
        if (!(z > 0.0f)) continue;
        points[3 * at] = rays[2 * at] * z;
        points[3 * at + 1] = rays[2 * at + 1] * z;
        points[3 * at + 2] = z;
    }
}

void kinect_v2_depth_rays_from_lens(float focal_x, float focal_y, float centre_x, float centre_y, uint32_t width,
                                    uint32_t height, std::vector<float>& rays) {
    rays.assign(size_t(width) * height * 2, 0.0f);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const size_t at = size_t(y) * width + x;
            rays[2 * at] = (float(x) - centre_x) / focal_x;
            rays[2 * at + 1] = -(float(y) - centre_y) / focal_y;
        }
}

uint32_t kinect_v2_short_tracking_id(uint64_t id) { return uint32_t(id) ? uint32_t(id) : uint32_t(id >> 32) | 1u; }

}
