// Our NuiTransformSmooth (nui_transform_smooth.h) against the real one: the
// Kinect for Windows SDK 1.8's Kinect10.dll exports
// NuiTransformSmooth(NUI_SKELETON_FRAME*, const NUI_TRANSFORM_SMOOTH_PARAMETERS*).
// The same synthetic skeleton frames go through both, with the default
// parameters and others, and every joint of every slot must agree within a
// tenth of a millimetre.
//
// No sensor is started: NuiInitialize is never called, only the DLL is
// loaded. When it is not installed, or its NuiTransformSmooth refuses to
// work without an initialized sensor, the test says so and is skipped (exit
// 77); the unit test (nui_transform_smooth_test) checks the algorithm on its
// own either way.
#include "nui_transform_smooth.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
// NuiSkeleton.h (SDK 1.8): the PC's own layout, little-endian.
struct NuiVector4 { float x, y, z, w; };
struct NuiSkeletonData {
    int32_t tracking_state;
    DWORD tracking_id, enrollment_index, user_index;
    NuiVector4 position;
    NuiVector4 positions[20];
    int32_t position_states[20];
    DWORD quality_flags;
};
struct NuiSkeletonFrame {
    LARGE_INTEGER timestamp;
    DWORD frame_number, flags;
    NuiVector4 floor_clip_plane, normal_to_gravity;
    NuiSkeletonData skeletons[6];
};
struct NuiTransformSmoothParameters { float smoothing, correction, prediction, jitter_radius, max_deviation_radius; };
static_assert(sizeof(NuiSkeletonData) == 436 && sizeof(NuiSkeletonFrame) == 2664, "SDK 1.8 layout");
using NuiTransformSmooth = HRESULT(WINAPI*)(NuiSkeletonFrame*, const NuiTransformSmoothParameters*);
constexpr int skipped = 77;

struct Body {
    uint32_t slot, id;
    sfr::SkeletonJoints joints;
    std::array<uint32_t, 20> states;
};

// A frame of the given bodies, in both forms.
void build(const std::vector<Body>& bodies, uint32_t number, NuiSkeletonFrame& sdk,
           std::array<sfr::NuiFrameSmoother::Slot, 6>& ours) {
    sdk = {};
    ours = {};
    sdk.timestamp.QuadPart = LONGLONG(number) * 33;
    sdk.frame_number = number;
    sdk.floor_clip_plane = {0.0f, 1.0f, 0.0f, 0.8f};
    sdk.normal_to_gravity = {0.0f, 1.0f, 0.0f, 0.0f};
    for (const Body& body : bodies) {
        NuiSkeletonData& data = sdk.skeletons[body.slot];
        data.tracking_state = 2;
        data.tracking_id = body.id;
        data.position = {body.joints[0][0], body.joints[0][1], body.joints[0][2], 1.0f};
        auto& slot = ours[body.slot];
        slot.tracking_state = 2;
        slot.tracking_id = body.id;
        for (int j = 0; j < 20; ++j) {
            data.positions[j] = {body.joints[j][0], body.joints[j][1], body.joints[j][2], 1.0f};
            data.position_states[j] = int32_t(body.states[j]);
            slot.joints[j] = body.joints[j];
            slot.states[j] = body.states[j];
        }
    }
}

struct Run {
    const char* name;
    bool explicit_parameters;
    NuiTransformSmoothParameters parameters;
};

// A standing body that sways and reaches, with sensor noise.
Body body_at(uint32_t slot, uint32_t id, uint32_t frame, std::mt19937& random, float x0) {
    std::normal_distribution<float> noise(0.0f, 0.006f);
    Body body{slot, id, {}, {}};
    const float t = float(frame) / 30.0f;
    const float sway = 0.12f * std::sin(t * 1.3f), reach = 0.25f * std::sin(t * 2.7f);
    for (int j = 0; j < 20; ++j) {
        body.joints[j] = {x0 + sway + 0.05f * float(j % 5) + noise(random), -0.6f + 0.06f * float(j) + noise(random),
                          2.4f + (j == 7 || j == 11 ? -reach : 0.0f) + noise(random)};
        body.states[j] = 2;
    }
    return body;
}

std::string text(HRESULT value) {
    char out[16];
    std::snprintf(out, sizeof out, "0x%08lX", static_cast<unsigned long>(value));
    return out;
}
}

int main() {
    HMODULE library = LoadLibraryW(L"Kinect10.dll");
    if (!library) {
        std::cout << "SKIPPED: Kinect10.dll (Kinect for Windows SDK/Runtime 1.8) is not installed\n";
        return skipped;
    }
    const auto smooth = reinterpret_cast<NuiTransformSmooth>(reinterpret_cast<void*>(GetProcAddress(library, "NuiTransformSmooth")));
    if (!smooth) {
        std::cout << "SKIPPED: Kinect10.dll exports no NuiTransformSmooth\n";
        return skipped;
    }
    // Clears the SDK's own per-slot history the documented way: a frame with
    // no tracked skeleton (our smoother is made anew for each run).
    const auto clear_sdk = [&] {
        NuiSkeletonFrame empty{};
        return smooth(&empty, nullptr);
    };
    {
        // Asked both ways (no frame tracked and no parameters; a tracked
        // skeleton and the defaults given) before giving up on it.
        const HRESULT empty = clear_sdk();
        std::mt19937 random(7);
        NuiSkeletonFrame sdk;
        std::array<sfr::NuiFrameSmoother::Slot, 6> slots;
        build({body_at(0, 1, 1, random, 0.0f)}, 1, sdk, slots);
        const NuiTransformSmoothParameters defaults{0.5f, 0.5f, 0.5f, 0.05f, 0.04f};
        const HRESULT tracked = smooth(&sdk, &defaults);
        if (FAILED(empty) && FAILED(tracked)) {
            std::cout << "SKIPPED: the SDK's NuiTransformSmooth refuses to work without NuiInitialize (it answered "
                      << text(empty) << " for an empty frame and " << text(tracked)
                      << " for a tracked one); no sensor is started here, so nui_transform_smooth_test checks the algorithm alone\n";
            return skipped;
        }
    }
    const std::vector<Run> runs = {
        {"defaults (no parameters)", false, {}},
        {"defaults, given", true, {0.5f, 0.5f, 0.5f, 0.05f, 0.04f}},
        {"light", true, {0.25f, 0.25f, 0.25f, 0.03f, 0.05f}},
        {"heavy", true, {0.8f, 0.2f, 1.0f, 0.1f, 0.1f}},
        {"no jitter radius", true, {0.5f, 0.5f, 0.5f, 0.0f, 0.04f}},
        {"wide", true, {0.6f, 0.4f, 0.5f, 0.5f, 0.5f}},
    };
    constexpr float tolerance = 1e-4f;  // metres
    int failures = 0;
    for (const Run& run : runs) {
        clear_sdk();
        sfr::NuiFrameSmoother ours;
        const sfr::NuiSmoothParameters parameters =
            run.explicit_parameters ? sfr::NuiSmoothParameters{run.parameters.smoothing, run.parameters.correction,
                                                               run.parameters.prediction, run.parameters.jitter_radius,
                                                               run.parameters.max_deviation_radius}
                                    : sfr::NuiSmoothParameters{};
        std::mt19937 random(1234);
        float worst = 0;
        uint32_t worst_frame = 0, compared = 0;
        HRESULT failed = S_OK;
        for (uint32_t number = 1; number <= 400; ++number) {
            std::vector<Body> bodies;
            // The first player throughout (slot 2), a second one (slot 4)
            // who steps out for a while and back in under a new id.
            Body first = body_at(2, 101, number, random, -0.3f);
            // Some joints inferred, one not tracked, one at the origin (lost).
            if (number % 7 == 0) first.states[7] = 1;
            if (number % 11 == 0) first.states[15] = 0;
            if (number >= 200 && number < 210) first.joints[19] = {0.0f, 0.0f, 0.0f};
            bodies.push_back(first);
            if (number < 120 || number >= 160) bodies.push_back(body_at(4, number < 120 ? 202 : 203, number, random, 0.4f));
            NuiSkeletonFrame sdk;
            std::array<sfr::NuiFrameSmoother::Slot, 6> slots;
            build(bodies, number, sdk, slots);
            const HRESULT result = smooth(&sdk, run.explicit_parameters ? &run.parameters : nullptr);
            if (FAILED(result)) { failed = result; break; }
            ours.smooth(slots, parameters);
            for (int s = 0; s < 6; ++s) {
                if (slots[s].tracking_state != 2) continue;
                for (int j = 0; j < 20; ++j) {
                    const NuiVector4& p = sdk.skeletons[s].positions[j];
                    const float d = (std::max)({std::fabs(p.x - slots[s].joints[j][0]), std::fabs(p.y - slots[s].joints[j][1]),
                                                std::fabs(p.z - slots[s].joints[j][2])});
                    ++compared;
                    if (d > worst) { worst = d; worst_frame = number; }
                }
            }
        }
        const bool ok = failed == S_OK && worst <= tolerance;
        std::printf("%-26s %s: %u joints compared, largest difference %.3g m (frame %u)%s\n", run.name, ok ? "agree" : "DIFFER",
                    compared, double(worst), worst_frame, failed == S_OK ? "" : (" ; the SDK answered " + text(failed)).c_str());
        if (!ok) ++failures;
    }
    // Informational: a new tracking id in a slot that stayed tracked. Ours
    // starts that slot over; does the SDK?
    {
        clear_sdk();
        sfr::NuiFrameSmoother ours;
        std::mt19937 random(99);
        float same_history = 0, ours_restart = 0;
        for (uint32_t number = 1; number <= 40; ++number) {
            NuiSkeletonFrame sdk;
            std::array<sfr::NuiFrameSmoother::Slot, 6> slots;
            build({body_at(1, number <= 20 ? 7 : 8, number, random, number <= 20 ? -0.5f : 0.5f)}, number, sdk, slots);
            const auto measured = slots[1].joints[3];
            smooth(&sdk, nullptr);
            ours.smooth(slots, {});
            if (number == 21) {
                same_history = std::fabs(sdk.skeletons[1].positions[3].x - measured[0]);
                ours_restart = std::fabs(slots[1].joints[3][0] - measured[0]);
            }
        }
        std::printf("tracking id change in a tracked slot: the SDK's first output is %.3g m from the measurement (0: it starts over), "
                    "ours %.3g m\n", double(same_history), double(ours_restart));
    }
    std::cout << (failures ? "FAILED: the SDK and ours differ\n" : "The SDK's NuiTransformSmooth and ours agree\n");
    return failures ? 1 : 0;
}
