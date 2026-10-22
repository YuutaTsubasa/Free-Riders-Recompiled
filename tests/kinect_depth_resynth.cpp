// Makes the depth a Kinect v2 gives the game again, offline, from a raw
// recording of what it measured (kinect_record_raw.h: sfr_kinect_probe --raw,
// or the game with SFR_KINECT_RECORD_RAW=1), through any view:
//
//   sfr_kinect_depth_resynth <v2raw.bin> --view fit|sensor|player|ideal [--distance m] [--height m]
//                            [--follow s] [--deadband m] [--fit-scale k] --out <depth.bin>
//
// The view state and the synthesis are the live sensor's own
// (KinectDepthViewer, kinect_v2_synthesize_depth; kinect_v2_resynth.h), so
// the same settings give the images the live path made. The output is a
// standard depth recording (SFRKDEP2, source 2: kinect_record_depth.h) that
// sfr_kinect_depth_features and sfr_kinect_depth_compare read as they read
// the probe's. Unset values are the environment variables' defaults
// (SFR_KINECT_V2_DEPTH_*); --fit-scale unset takes the recording's own lens.
#include "kinect_record_depth.h"
#include "kinect_record_raw.h"
#include "kinect_v2_resynth.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string input, output;
    const char* view = nullptr;
    const char* distance = nullptr;
    const char* height = nullptr;
    const char* follow = nullptr;
    const char* deadband = nullptr;
    const char* fit_scale = nullptr;
    bool usage = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool more = i + 1 < argc;
        if (arg == "--view" && more) view = argv[++i];
        else if (arg == "--distance" && more) distance = argv[++i];
        else if (arg == "--height" && more) height = argv[++i];
        else if (arg == "--follow" && more) follow = argv[++i];
        else if (arg == "--deadband" && more) deadband = argv[++i];
        else if (arg == "--fit-scale" && more) fit_scale = argv[++i];
        else if (arg == "--out" && more) output = argv[++i];
        else if (!arg.empty() && arg[0] != '-' && input.empty()) input = arg;
        else usage = true;
    }
    const std::string name = view ? view : "";
    if (usage || input.empty() || output.empty() ||
        (name != "fit" && name != "sensor" && name != "player" && name != "ideal")) {
        std::cerr << "usage: sfr_kinect_depth_resynth <v2raw.bin> --view fit|sensor|player|ideal [--distance m] [--height m]\n"
                     "                                [--follow s] [--deadband m] [--fit-scale k] --out <depth.bin>\n";
        return 2;
    }
    sfr::KinectRawReader reader;
    if (!reader.open(input)) {
        std::cerr << input << ": " << reader.error() << '\n';
        return 1;
    }
    const sfr::KinectDepthSettings settings =
        sfr::KinectDepthSettings::parse(view, distance, height, fit_scale, follow, deadband);
    sfr::KinectV2Resynth resynth(settings, reader.header());
    std::printf("%s: %ux%u, rays %s, lens focal %.1f\n", input.c_str(), reader.header().width, reader.header().height,
                sfr::kinect_raw_rays_name(reader.header().rays_from), double(reader.header().lens[1]));
    if (!resynth.warning().empty()) std::printf("note: %s\n", resynth.warning().c_str());
    std::printf("view=%s", settings.view_name());
    if (settings.mode == sfr::KinectDepthViewMode::fit) std::printf(" fit_scale=%.4f", double(resynth.viewer().fit_scale()));
    if (settings.mode == sfr::KinectDepthViewMode::player || settings.mode == sfr::KinectDepthViewMode::ideal)
        std::printf(" distance=%.2f height=%.2f", double(settings.distance), double(settings.height));
    if (settings.mode == sfr::KinectDepthViewMode::ideal)
        std::printf(" follow=%.2f deadband=%.2f", double(settings.follow), double(settings.deadband));
    std::printf("\n");
    // A made image is never larger than the raw frame it comes from (153,600
    // bytes at most against about 434,000): the raw file's size bounds the
    // output, and the free space warning means something.
    // Offline: wait for the disk rather than drop an image.
    sfr::KinectRecordQueue::process().set_blocking(true);
    std::error_code error;
    const uint64_t raw_bytes = std::filesystem::file_size(input, error);
    sfr::KinectDepthRecordFile file(output, (error ? (64ull << 30) : raw_bytes) + (1ull << 20), 1);
    sfr::KinectRawFrame frame;
    sfr::KinectImage image;
    uint64_t frames = 0, mapped = 0, written = 0;
    double valid = 0, players = 0;
    while (reader.next(frame)) {
        const sfr::KinectDepthStats stats = resynth.make(frame, image);
        ++frames;
        mapped += (frame.flags & sfr::kinect_raw_flag_mapped) ? 1 : 0;
        valid += stats.valid;
        players += stats.players;
        sfr::KinectDepthRecordFrame record;
        record.steady_us = image.steady_us;
        record.handed_us = image.steady_us;
        record.sdk_time = image.sdk_time;
        record.number = image.number;
        record.source = sfr::KinectDepthSource::v2;
        record.origin = image.view_origin;
        record.z_scale = image.view_z_scale;
        sfr::kinect_depth_frame_pixels(record, image.pixels.data(), image.width, image.height);
        if (file.write(record)) ++written;
    }
    // Everything on disk before the summary (no time limit offline).
    sfr::KinectRecordQueue::process().drain(&file, std::chrono::hours(24));
    if (reader.truncated()) std::printf("the last frame was cut short: left out\n");
    if (!reader.error().empty()) std::printf("stopped at a broken frame: %s\n", reader.error().c_str());
    const double n = frames ? double(frames) : 1.0;
    std::printf("frames=%llu (mapper points %llu, lens %llu) written=%llu to %s; valid %.1f%%, player pixels %.0f\n",
                (unsigned long long)frames, (unsigned long long)mapped, (unsigned long long)(frames - mapped),
                (unsigned long long)written, output.c_str(), 100.0 * valid / n / (320.0 * 240.0), players / n);
    if (settings.mode == sfr::KinectDepthViewMode::ideal) {
        const sfr::KinectIdealState& state = resynth.viewer().ideal();
        const sfr::KinectDepthView& v = resynth.viewer().view();
        float pitch = 0, roll = 0;
        sfr::kinect_floor_tilt(state.floor, pitch, roll);
        std::printf("ideal camera at the end: %.3f,%.3f,%.3f level=%d pitch=%.1f roll=%.1f reference=%.3f,%.3f\n",
                    double(v.origin[0]), double(v.origin[1]), double(v.origin[2]), state.floor_known ? 1 : 0,
                    double(pitch), double(roll), double(state.reference[0]), double(state.reference[1]));
    }
    return frames && written == frames ? 0 : 1;
}
