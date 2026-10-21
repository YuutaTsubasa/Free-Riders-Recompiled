#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <vector>

namespace sfr {

// The title's own depth view, again, outside the game: what its two passes
// over the 320x240 depth-and-player-index image compute for one player, so
// that a recorded image (kinect_record_depth.h) gives the very numbers the
// race's detectors read (docs/kinect-sensor.md, "深度檢視的特徵").
//
// The depth view (82439530, on the Kinect manager's worker thread) runs once
// per new depth image for each of the manager's two player slots (+7740,
// +7744; a skeleton slot 0..5, or negative for none):
//   82439998(slot, manager+7584, image, width, height)  pass one
//   82439D80(slot, record+640, manager+7584+8*slot, image, width, height)
// where record = manager + 2784 + 800*slot is the player's body record (its
// +768 holds the skeleton, +784 the slot: 82438A98). Both are VMX code over
// eight pixels at a time; the image is read as one packed array of uint16,
// width/8 vectors a row (width a multiple of 8; 320 is).
//
// A pixel is the player's when (pixel & 7) == slot + 1: the mask 0xFFF8 is
// cleared (vandc with a splat of 0xFFF8: 82439BA4, 82439F80, 8243A028,
// 8243A430) and the rest compared with a splat of slot + 1 from a table of
// 1..6 on the stack (vcmpequh: 82439BC4, 82439FA0, 8243A048, 8243A434). Its
// depth is pixel & 0xFFF8 (millimetres << 3, never shifted down).
//
// Pass one, 82439998 (82439998..82439D7C):
//   for every row r and column c of a player pixel: the sum of the columns,
//   of the rows and the count, in eight 16-bit lanes (column c in lane c % 8)
//   that saturate (vadduhs, 82439BDC..82439BE4; a row's lane sums are added
//   into 32-bit words after the row, 82439BFC..82439C20), the lanes'
//   smallest and largest column (vminuh/vmaxuh and vsel,
//   82439BB4..82439BD8), the first and last row with one (82439C08..82439C34).
//   centroid = (column sum / count, row sum / count), unsigned division
//   (divwu, 82439CB4, 82439CDC), or (width/2, height/2) with no pixel
//   (82439CE8..82439CF4); stored as two words at manager+7584+8*slot
//   (82439CB8, 82439CE0). The smallest column and row at manager+7632+8*slot
//   (stwx at 82439D50, stw at 82439D28), the largest at manager+7680+8*slot
//   (stwx at 82439D78, stw at 82439D64). The largest column is not quite
//   that: the eight lanes' largest are merged pairwise (lane e with e + 4)
//   by vminuw (82439D30) where the smallest column's code uses vminuw too
//   (82439D1C), and only then the largest of the four taken (vmaxuw,
//   82439D60, 82439D6C); so it is the largest column whose partner lane
//   reached at least as far. Without a pixel: smallest column = width,
//   smallest row = height, largest = 0, 0. The count itself is not kept.
// Pass two, 82439D80 (82439D80..8243A4A4), with the centroid (cc, cr) of
// pass one: rows 0..cr-1 are the upper part (82439F68..82439FF0), rows
//   cr..height-1 the lower (8243A010..8243A098); a pixel is on the left
//   when its column < cc (vcmpgtuh cc > column, 82439F90, 8243A038), on the
//   right otherwise. Per part and side: the player's pixel count (16-bit
//   saturating lanes) and depth sum (32-bit words). Then (8243A09C..):
//     total = ((upper right + upper left) + lower right) + lower left, float
//     (fadds 8243A234, 8243A240, 8243A260)
//     if total < float((width*height) >> 9) (150 for 320x240; mullw
//     8243A1D8, rlwinm 8243A1E4, fcmpu 8243A294):
//       record+640..+655 = 0 (stvx v0, 8243A29C) and nothing else is
//       written: the means, near count and average keep an older image's.
//     else
//       +640 lower left count, +644 lower right, +648 upper left, +652
//       upper right (floats; stvx at 8243A2D4)
//       +656.. the same four parts' mean depth (sum * refined 1/count,
//       vrefp and two Newton steps, 8243A300..8243A3A0; 65528.0, the
//       constant at 821AC140, for a part with no pixel; stvx at 8243A3A4)
//       +676 average = (((UR + UL) + LR) + LL) * 0.25 (8243A3A8..8243A3C4,
//       stfs at 8243A3C8)
//       +672 near: the player's pixels whose depth is less than 600 (75 mm)
//       from the average truncated (fctidz 8243A3CC) and & 0xFFF8, compared
//       as signed 16 bits (vcmpgtsh 8243A454; the loop 8243A42C..8243A468
//       goes over the whole image as width*height/8 vectors); stw 8243A4A0.
// The race's lean reads +640 (the image's lower left, the player's right
// as they face the sensor) against +644 (822C6200; nui_race.h).
//
// Every float operation is done one at a time as the recompiled code does
// it (no fused multiply-add), so the results are the game's bit for bit;
// kinect_depth_view_guest_test runs the generated code of both functions on
// the same images to check that.

// Pass one's output for one slot.
struct KinectDepthCentroid {
    uint32_t column = 0, row = 0;          // manager+7584+8*slot, +4
    uint32_t min_column = 0, min_row = 0;  // manager+7632+8*slot, +4
    uint32_t max_column = 0, max_row = 0;  // manager+7680+8*slot, +4
    uint32_t pixels = 0;                   // the count (not kept by the game)
};

// Pass two's output (record +640..+679) for one slot.
struct KinectDepthSplit {
    // Whether the image had enough of the player for more than the zeroed
    // counts (total >= (width*height) >> 9); false leaves means, near and
    // average as the previous image's, which the tool does not know.
    bool written = false;
    // lower left, lower right, upper left, upper right (+640, +644, +648, +652)
    std::array<float, 4> counts{};
    // the same parts' mean depth, millimetres << 3 (+656, +660, +664, +668)
    std::array<float, 4> means{};
    uint32_t near_count = 0;  // +672 (not "near": a macro in <windows.h>)
    float average = 0;    // +676
};
enum KinectDepthPart : uint32_t { lower_left = 0, lower_right = 1, upper_left = 2, upper_right = 3 };

KinectDepthCentroid kinect_depth_view_centroid(const uint16_t* pixels, uint32_t width, uint32_t height, uint32_t slot);
KinectDepthSplit kinect_depth_view_split(const uint16_t* pixels, uint32_t width, uint32_t height, uint32_t slot,
                                         uint32_t centroid_column, uint32_t centroid_row);

// Both passes for one slot, and the image's share of pixels without a depth
// (depth 0 after the mask) and its pixels of any player, for a summary.
struct KinectDepthFeatures {
    uint32_t slot = 0;
    KinectDepthCentroid centroid;
    KinectDepthSplit split;
};
KinectDepthFeatures kinect_depth_view_features(const uint16_t* pixels, uint32_t width, uint32_t height, uint32_t slot);
struct KinectDepthImageStats {
    float invalid = 0;            // pixels with no depth, 0..1
    std::array<uint32_t, 7> players{};  // pixels of each player index (0 none)
};
KinectDepthImageStats kinect_depth_image_stats(const uint16_t* pixels, size_t count);

}
