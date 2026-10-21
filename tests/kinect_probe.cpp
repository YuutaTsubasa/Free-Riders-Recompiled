// Checks a real Kinect without starting the game: opens the first sensor
// through the Kinect for Windows runtime and prints what it tracks for ten
// seconds.
//
//   sfr_kinect_probe [seconds] [--v1 | --v2 | --both] [--record prefix [--depth] [--raw]] [--images [prefix]] [--game-depth]
//                    [--smooth]
//
// --v1 or --v2 opens that kind only, --both opens a Kinect v1 and a Kinect
// v2 together (each on its own runtime) and carries on with whichever
// opened, saying which. --record writes every skeleton frame of every open
// sensor to <prefix>-skeleton.csv (kinect_record.h), each stamped with the
// host's steady clock and the sensor's own timestamp, with the share of its
// depth image that has no depth (a v2's depth reader is opened for it).
// --depth (or SFR_KINECT_RECORD_DEPTH=1) also writes every depth image each
// sensor gives the game, as the game would get it, to
// <prefix>-v1-depth.bin and <prefix>-v2-depth.bin (kinect_record_depth.h):
// a v1's own depth and player index, the one a v2 makes (its fit view).
// --raw (or SFR_KINECT_RECORD_RAW=1) also writes what a Kinect v2 measured for
// each image it makes, to <prefix>-v2raw.bin (kinect_record_raw.h), so the
// depth for the game can be made again offline through any view
// (sfr_kinect_depth_resynth); the sensor writes it from the frames it makes the
// image from.
// With --images it also saves the first colour and depth image each sensor
// gives, as the launcher's preview shows them, to <prefix>colour.bmp and
// <prefix>depth.bmp (prefix "kinect_" by default, with "v1_"/"v2_" added
// when both are open), and prints where the head falls in each.
// --smooth (or SFR_KINECT_SMOOTH=1) smooths the skeletons in the backends
// with the NUI library's filter (NuiTransformSmooth, kinect_smoothing.h), as
// the game smooths them itself; recordings still take them before that.
// --game-depth reads the depth a Kinect v2 makes for the game (always made,
// kinect_v2_depth.h) as the game does (game_image): twice a
// second how many images were made, their share of pixels with a depth and
// the players' pixels, and with --images the first one as
// <prefix>game_depth.bmp.
#include "kinect_record.h"
#include "kinect_record_depth.h"
#include "kinect_record_raw.h"
#include "kinect_sensor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// 32-bit B, G, R, unused rows, top-down.
bool write_bmp(const std::string& path, const std::vector<uint8_t>& bgrx, uint32_t width, uint32_t height) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint32_t pixels = width * height * 4, size = 54 + pixels;
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    const auto put32 = [&](int at, uint32_t value) {
        header[at] = uint8_t(value); header[at + 1] = uint8_t(value >> 8);
        header[at + 2] = uint8_t(value >> 16); header[at + 3] = uint8_t(value >> 24);
    };
    put32(2, size); put32(10, 54); put32(14, 40);
    put32(18, width);
    put32(22, uint32_t(-int32_t(height)));  // negative: rows top-down
    header[26] = 1; header[28] = 32;
    put32(34, pixels);
    out.write(reinterpret_cast<const char*>(header), sizeof header);
    out.write(reinterpret_cast<const char*>(bgrx.data()), pixels);
    return bool(out);
}

// Depth and player index as the preview shades it: near bright, nothing
// measured black, players tinted red.
std::vector<uint8_t> shade_depth(const sfr::KinectImage& depth) {
    std::vector<uint8_t> out(size_t(depth.width) * depth.height * 4);
    for (size_t i = 0; i < size_t(depth.width) * depth.height; ++i) {
        const uint16_t value = uint16_t(depth.pixels[2 * i] | depth.pixels[2 * i + 1] << 8);
        const uint32_t millimetres = value >> 3, player = value & 7;
        const uint32_t shade = millimetres == 0 ? 0 :
            255 - std::min<uint32_t>(255, uint32_t(std::max(0, int(millimetres) - 500)) * 255 / 3500);
        out[4 * i] = uint8_t(player ? shade / 3 : shade);
        out[4 * i + 1] = uint8_t(player ? shade / 3 : shade);
        out[4 * i + 2] = uint8_t(shade);
    }
    return out;
}

struct Probe {
    std::unique_ptr<sfr::KinectSensor> sensor;
    std::string tag;  // "v1", "v2", or empty for the first found
    std::string prefix;
    sfr::KinectFrame frame;
    sfr::KinectImage colour, depth, game_depth, recorded_depth;
    bool colour_saved = false, depth_saved = false, game_depth_saved = false;
    uint64_t game_depth_frames = 0, depth_recorded = 0;
    uint64_t shown = 0, frames = 0, with_body = 0;
};
}

int main(int argc, char** argv) {
    int seconds = 10;
    bool images = false;
    std::string prefix = "kinect_", record;
    int models = 0;  // 0 the first found, 1 v1, 2 v2, 3 both
    bool game_depth = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--images") {
            images = true;
            if (i + 1 < argc && argv[i + 1][0] != '-' && std::atoi(argv[i + 1]) == 0) prefix = argv[++i];
        } else if (arg == "--record" && i + 1 < argc) {
            record = argv[++i];
        } else if (arg == "--v1") {
            models = 1;
        } else if (arg == "--v2") {
            models = 2;
        } else if (arg == "--both") {
            models = 3;
        } else if (arg == "--game-depth") {
            game_depth = true;
        } else if (arg == "--depth") {
            sfr::kinect_record_depth_enable();
        } else if (arg == "--raw") {
            sfr::kinect_record_raw_enable();
        } else if (arg == "--smooth") {
            // Read by each backend as it opens (kinect_host_smoothing).
#ifdef _WIN32
            _putenv_s("SFR_KINECT_SMOOTH", "1");
#else
            setenv("SFR_KINECT_SMOOTH", "1", 1);
#endif
        } else {
            seconds = std::atoi(argv[i]);
        }
    }
    // Before any sensor opens: their workers check once.
    if (!record.empty()) sfr::kinect_record_start(record);
    const bool recording = !sfr::kinect_record_prefix().empty();
    std::vector<Probe> probes;
    if (models == 0) {
        std::string why;
        auto sensor = sfr::KinectSensor::open(&why);
        if (!sensor) {
            std::cout << "no sensor: " << why << '\n';
            return 1;
        }
        probes.push_back({std::move(sensor), "", prefix});
    } else {
        // Each kind on its own: one missing (or a runtime that will not
        // share the machine with the other) leaves the other to record.
        for (int model = 1; model <= 2; ++model) {
            if (!(models & model)) continue;
            const std::string tag = model == 1 ? "v1" : "v2";
            std::string why;
            auto sensor = sfr::KinectSensor::open_model(model, &why);
            std::cout << "Kinect " << tag << ": " << (sensor ? "open" : "not open (" + why + ")") << std::endl;
            if (sensor) probes.push_back({std::move(sensor), tag, models == 3 ? prefix + tag + "_" : prefix});
        }
        if (probes.empty()) {
            std::cout << "no sensor opened" << '\n';
            return 1;
        }
    }
    for (const Probe& probe : probes)
        std::cout << probe.sensor->model() << " open; stand in front of it"
                  << (game_depth ? std::string(" (depth for the game: ") +
                                       (probe.sensor->has_depth_stream() ? "yes" : "no") + ")"
                                 : std::string())
                  << std::endl;
    if (recording)
        std::cout << "recording every frame to " << sfr::kinect_record_prefix() << "-skeleton.csv for " << seconds
                  << " s" << std::endl;
    // Each sensor's depth file, named after its kind.
    const bool recording_depth = recording && sfr::kinect_record_depth_enabled();
    std::vector<sfr::KinectDepthRecordFile*> depth_files;
    for (const Probe& probe : probes) {
        const std::string tag = probe.tag.empty() ? (std::string(probe.sensor->model()) == "Kinect v2" ? "v2" : "v1")
                                                  : probe.tag;
        sfr::KinectDepthRecordFile* file = recording_depth ? sfr::kinect_record_depth_file(tag) : nullptr;
        depth_files.push_back(file);
        if (file)
            std::cout << "recording the depth images the game would get to " << file->path()
                      << (probe.sensor->has_depth_stream() ? "" : " (this sensor has no depth for the game: none)")
                      << std::endl;
    }
    // A Kinect v2's raw frames: written by the sensor itself.
    const bool recording_raw = recording && sfr::kinect_record_raw_enabled();
    if (recording_raw)
        std::cout << "recording what a Kinect v2 measures for each depth image it makes to "
                  << sfr::kinect_record_raw_path(sfr::kinect_record_prefix()) << std::endl;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        bool any = false;
        for (size_t index = 0; index < probes.size(); ++index) {
            Probe& probe = probes[index];
            auto& sensor = probe.sensor;
            // The depth image the game would be handed (game_image: a v1's
            // own, a v2's made one), every one, into its file.
            if (sfr::KinectDepthRecordFile* file = depth_files[index];
                file && sensor->has_depth_stream() &&
                sensor->game_image(sfr::KinectImageKind::depth_and_player, probe.recorded_depth) &&
                probe.recorded_depth.bytes_per_pixel == 2) {
                const sfr::KinectImage& image = probe.recorded_depth;
                sfr::KinectDepthRecordFrame record;
                record.steady_us = image.steady_us;
                record.handed_us = sfr::kinect_record_now_us();
                record.sdk_time = image.sdk_time;
                record.number = image.number;
                record.sdk_frame = image.sdk_frame;
                record.source = std::string(sensor->model()) == "Kinect v2" ? sfr::KinectDepthSource::v2
                                                                             : sfr::KinectDepthSource::v1;
                record.origin = image.view_origin;
                record.z_scale = image.view_z_scale;
                sfr::kinect_depth_frame_pixels(record, image.pixels.data(), image.width, image.height);
                if (file->write(record)) ++probe.depth_recorded;
                any = true;
            }
            // Recording keeps a v2's depth reader open (its share of pixels
            // without a depth goes in the file); a v1 has it open already.
            const bool want_depth = images || recording;
            if (want_depth && sensor->image(sfr::KinectImageKind::depth_and_player, probe.depth) && images &&
                !probe.depth_saved && probe.depth.bytes_per_pixel == 2) {
                const std::string path = probe.prefix + "depth.bmp";
                probe.depth_saved = write_bmp(path, shade_depth(probe.depth), probe.depth.width, probe.depth.height);
                std::cout << probe.tag << (probe.tag.empty() ? "" : " ") << "depth " << probe.depth.width << 'x'
                          << probe.depth.height << (probe.depth_saved ? " saved to " : " NOT saved to ") << path << std::endl;
            }
            if (images && !probe.colour_saved && sensor->image(sfr::KinectImageKind::colour, probe.colour) &&
                probe.colour.bytes_per_pixel == 4) {
                const std::string path = probe.prefix + "colour.bmp";
                probe.colour_saved = write_bmp(path, probe.colour.pixels, probe.colour.width, probe.colour.height);
                std::cout << probe.tag << (probe.tag.empty() ? "" : " ") << "colour " << probe.colour.width << 'x'
                          << probe.colour.height << (probe.colour_saved ? " saved to " : " NOT saved to ") << path
                          << std::endl;
            }
            if (game_depth && sensor->has_depth_stream() &&
                sensor->game_image(sfr::KinectImageKind::depth_and_player, probe.game_depth)) {
                ++probe.game_depth_frames;
                if (images && !probe.game_depth_saved) {
                    const std::string path = probe.prefix + "game_depth.bmp";
                    probe.game_depth_saved = write_bmp(path, shade_depth(probe.game_depth), probe.game_depth.width,
                                                       probe.game_depth.height);
                    std::cout << probe.tag << (probe.tag.empty() ? "" : " ") << "game depth " << probe.game_depth.width
                              << 'x' << probe.game_depth.height << (probe.game_depth_saved ? " saved to " : " NOT saved to ")
                              << path << std::endl;
                }
            }
            if (!sensor->next(probe.frame)) continue;
            any = true;
            sfr::KinectFrame& frame = probe.frame;
            ++probe.frames;
            if (!frame.bodies.empty()) ++probe.with_body;
            const float gravity_y = frame.gravity[1];
            const sfr::KinectFrame& given = frame;
            if (frame.number - probe.shown < 15) continue;  // twice a second
            probe.shown = frame.number;
            if (!probe.tag.empty()) std::cout << probe.tag << ' ';
            std::cout << "frame " << frame.number << " floor=" << frame.floor_plane[1] << ',' << frame.floor_plane[3]
                      << " gravity_y=" << gravity_y << " bodies=" << frame.bodies.size();
            if (game_depth && !probe.game_depth.pixels.empty()) {
                // The newest image the game would get: made, with a depth, players.
                uint32_t valid = 0, players = 0;
                for (size_t i = 0; i + 1 < probe.game_depth.pixels.size(); i += 2) {
                    const uint16_t value = uint16_t(probe.game_depth.pixels[i] | probe.game_depth.pixels[i + 1] << 8);
                    if (value) ++valid;
                    if (value & 7) ++players;
                }
                std::cout << " game_depth_frames=" << probe.game_depth_frames
                          << " valid=" << double(valid) / double(probe.game_depth.pixels.size() / 2)
                          << " player_pixels=" << players;
            }
            for (size_t b = 0; b < frame.bodies.size(); ++b) {
                const auto& body = frame.bodies[b];
                const auto& hand = body.joints[sfr::nui_joint::hand_right];
                std::cout << " [id " << body.tracking_id << " at " << body.position[0] << ',' << body.position[2]
                          << " right hand " << hand[0] << ',' << hand[1] << ',' << hand[2];
                // Where the preview draws the head, when the sensor places it
                // itself (a v2): depth image 320x240, colour image 640x480.
                sfr::KinectImagePoint in_depth, in_colour;
                const auto& head = given.bodies[b].joints[sfr::nui_joint::head];
                if (images && sensor->image_point(sfr::KinectImageKind::depth_and_player, head, in_depth) &&
                    sensor->image_point(sfr::KinectImageKind::colour, head, in_colour))
                    std::cout << " head depth " << in_depth.x << ',' << in_depth.y << " colour " << in_colour.x << ','
                              << in_colour.y;
                std::cout << ']';
            }
            std::cout << std::endl;
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    for (const Probe& probe : probes) {
        std::cout << probe.sensor->model() << ": " << probe.frames << " frames, " << probe.with_body << " with a body";
        if (images)
            std::cout << "; images: colour " << (probe.colour_saved ? "saved" : "none") << ", depth "
                      << (probe.depth_saved ? "saved" : "none");
        if (game_depth) std::cout << "; depth images for the game: " << probe.game_depth_frames;
        if (recording_depth) std::cout << "; depth images recorded: " << probe.depth_recorded;
        std::cout << std::endl;
    }
    if (recording_raw)
        if (const sfr::KinectRawRecordFile* raw = sfr::kinect_record_raw_file())
            std::cout << "Kinect v2 raw frames recorded: " << raw->frames_written() << " to " << raw->path() << std::endl;
    return 0;
}
