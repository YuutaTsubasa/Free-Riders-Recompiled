#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "kinect_sensor.h"
#include "nui_analysis_trace.h"
#include "nui_skeleton.h"
#include "nui_transform_smooth.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>

// NuiTransformSmooth (827701A0) as the title calls it: once for every new
// skeleton frame, from the Kinect manager's update (82438D80 at 82438E58),
// on its own copy of the frame (manager +16) with the parameters its
// constructor wrote at manager +2752 (82438A98: 0.5, 0.5, 0.5, 0.05, 0.04),
// before it turns the joints into its body records. So the title's menu
// cursor, its race detectors and its body measurements were all written for
// smoothed skeletons.
//
// The original does nothing here: its first test is the flag at 82B5D2AC,
// which only the original NuiInitialize (8276FD10) sets, and that never runs
// (nui_hooks.cpp answers NuiInitialize itself), so it answers E_UNEXPECTED
// (0x8000FFFF) and the title, which does not look at the answer, read its
// skeletons unsmoothed. Now the library's filter runs natively
// (nui_transform_smooth.h) with the parameters the title passes, on the
// title's frame, and the answer is S_OK.
//
// Only a real Kinect's skeletons (v1 or v2: nui_body_from_sensor) are
// smoothed. The console smoothed every skeleton the runtime tracked, but
// the emulated ones (the pad's player, a webcam's pose) are not a sensor's:
// the pad's are exact and step only as the pad does, and a webcam's pose is
// smoothed already (pose_smoothing.h); filtering them again would only add
// latency (a frame or so) to every menu and race input. They are left as
// they are, and the answer is S_OK all the same.
//
// A real skeleton is smoothed exactly once: SFR_KINECT_SMOOTH=1 (the
// backends' own filter, for sfr_kinect_probe and tests; off by default)
// leaves this one out. SFR_NUI_SMOOTH=0 runs the original instead (which
// does nothing, as before), to compare.
//
// SFR_NUI_ANALYSIS_TRACE=1 (nui_analysis_trace.h): at most once a second,
// NUI_TRACE_SMOOTH with the mode, the answer, the library's flag, the
// parameters and the tracked skeletons.

namespace {
constexpr uint32_t nui_initialized_flag = 0x82B5D2AC;

float guest_float(sfr::GuestMemory& memory, uint64_t address) {
    return std::bit_cast<float>(memory.load<uint32_t>(address));
}

bool native_smoothing() {
    static const bool native = [] {
        const char* text = std::getenv("SFR_NUI_SMOOTH");
        return !text || *text != '0';
    }();
    return native;
}

sfr::NuiFrameSmoother smoother;
}

PPC_FUNC_IMPL(__imp__sub_827701A0);
SFR_INPUT_HOOK(sub_827701A0) {
    sfr::enter_function(ctx, "sub_827701A0", 0x827701A0);
    const uint32_t frame = ctx.r3.u32, parameters = ctx.r4.u32;
    auto& memory = *sfr::active_memory;
    const char* mode = "native";
    if (!native_smoothing()) {
        mode = "original";
        __imp__sub_827701A0(ctx, base);
    } else if (!sfr::nui_body_from_sensor()) {
        mode = "emulated";  // left as it is (see above)
        smoother.reset();
        ctx.r3.u64 = 0;
    } else if (sfr::kinect_host_smoothing()) {
        mode = "host";  // smoothed by the backend already
        smoother.reset();
        ctx.r3.u64 = 0;
    } else {
        try {
            ctx.r3.u64 = sfr::nui_transform_smooth_guest(memory, frame, parameters, smoother);
        } catch (const std::exception& error) {
            static bool told = false;
            if (!told) std::cerr << "NUI_TRANSFORM_SMOOTH unreadable frame=0x" << std::hex << frame << std::dec << ": "
                                 << error.what() << '\n';
            told = true;
            ctx.r3.u64 = 0x80004003u;  // E_POINTER
        }
    }
    {
        // Once, and whenever it changes: which way the title's skeletons go.
        static char told = 0;
        if (told != mode[0]) {
            told = mode[0];
            std::cerr << "NUI_TRANSFORM_SMOOTH mode=" << mode << " result=0x" << std::hex << ctx.r3.u32 << std::dec
                      << (mode[0] == 'n' ? " (the library's filter, natively, with the title's parameters)"
                          : mode[0] == 'e' ? " (an emulated skeleton: not smoothed)"
                          : mode[0] == 'h' ? " (SFR_KINECT_SMOOTH=1 smoothed it in the backend)"
                                           : " (SFR_NUI_SMOOTH=0: the original, which does nothing uninitialized)")
                      << '\n';
        }
    }
    static sfr::NuiTraceLimiter limiter("SMOOTH", 1.0, 4000);
    if (!limiter.take()) return;
    std::ostringstream line;
    line << "NUI_TRACE_SMOOTH mode=" << mode << " result=0x" << std::hex << ctx.r3.u32 << std::dec << " initialized="
         << (memory.readable(nui_initialized_flag, 4) ? memory.load<uint32_t>(nui_initialized_flag) : ~0u)
         << " frame=0x" << std::hex << frame << " params=0x" << parameters << std::dec;
    if (parameters && memory.readable(parameters, 20)) {
        line << " smoothing=" << guest_float(memory, parameters) << " correction=" << guest_float(memory, parameters + 4)
             << " prediction=" << guest_float(memory, parameters + 8)
             << " jitter_radius=" << guest_float(memory, parameters + 12)
             << " max_deviation_radius=" << guest_float(memory, parameters + 16);
    }
    if (frame && memory.readable(frame, sfr::nui_skeleton_frame_size)) {
        uint32_t tracked = 0;
        for (uint32_t slot = 0; slot < 6; ++slot)
            tracked += memory.load<uint32_t>(uint64_t(frame) + sfr::nui_skeleton_data_offset +
                                             uint64_t(slot) * sfr::nui_skeleton_data_size) == sfr::nui_tracked;
        line << " frame_number=" << memory.load<uint32_t>(uint64_t(frame) + 8) << " tracked=" << tracked;
    }
    std::cerr << line.str() << '\n';
}
