#include "kinect_v2_image.h"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close_to(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

uint32_t colour_at(const sfr::KinectImage& image, uint32_t x, uint32_t y) {
    const uint8_t* p = &image.pixels[(size_t(y) * image.width + x) * 4];
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint16_t depth_at(const sfr::KinectImage& image, uint32_t x, uint32_t y) {
    const uint8_t* p = &image.pixels[(size_t(y) * image.width + x) * 2];
    return uint16_t(p[0] | p[1] << 8);
}

void fits_keep_the_shape() {
    // 16:9 colour in 4:3: a third the size, 60-pixel bars above and below.
    const sfr::KinectFit colour = sfr::kinect_fit(1920, 1080, 640, 480);
    require(close_to(colour.scale, 1.0f / 3.0f, 1e-6f), "colour scale");
    require(close_to(colour.offset_x, 0) && close_to(colour.offset_y, 60), "colour bars above and below");
    // 512x424 depth in 320x240: as tall, with bars at the sides.
    const sfr::KinectFit depth = sfr::kinect_fit(512, 424, 320, 240);
    require(close_to(depth.scale, 240.0f / 424.0f, 1e-6f), "depth scale");
    require(close_to(depth.offset_y, 0) && close_to(depth.offset_x, (320.0f - 512.0f * 240.0f / 424.0f) / 2), "depth bars at the sides");
    require(depth.offset_x > 15 && depth.offset_x < 15.2f, "about 15 pixels each side");
}

using View = sfr::KinectV2View;
using Kind = sfr::KinectImageKind;

void crops_are_the_common_view() {
    // The same formulas as the code, from the lenses' tangent space.
    const float colour_half_y = 540.0f / 1060.0f, depth_half_y = 212.0f / 366.0f;  // 0.509, 0.579
    const float colour_half_x = 960.0f / 1060.0f, depth_half_x = 256.0f / 366.0f;  // 0.906, 0.699
    const float half_y = std::min(colour_half_y, depth_half_y);
    const float half_x = std::min(colour_half_x, depth_half_x);
    require(close_to(half_x, 0.699f, 1e-3f) && close_to(half_y, 0.509f, 1e-3f), "the intersection of the fields of view");
    const sfr::KinectV2Crop colour = sfr::kinect_v2_crop(Kind::colour, View::common);
    require(close_to(colour.x, 960 - half_x * 1060, 0.05f) && close_to(colour.y, 540 - half_y * 1060, 0.05f) &&
                close_to(colour.width, 2 * half_x * 1060, 0.1f) && close_to(colour.height, 2 * half_y * 1060, 0.1f),
            "colour crop from the lens");
    require(colour.x > 218 && colour.x < 219.5f && colour.x + colour.width > 1700 && colour.x + colour.width < 1702 &&
                close_to(colour.y, 0, 0.1f) && close_to(colour.height, 1080, 0.1f),
            "colour crop about x 219..1701 (1482 px), all 1080 rows");
    const sfr::KinectV2Crop depth = sfr::kinect_v2_crop(Kind::depth_and_player, View::common);
    require(close_to(depth.x, 256 - half_x * 366, 0.05f) && close_to(depth.y, 212 - half_y * 366, 0.05f) &&
                close_to(depth.width, 2 * half_x * 366, 0.1f) && close_to(depth.height, 2 * half_y * 366, 0.1f),
            "depth crop from the lens");
    require(close_to(depth.x, 0, 0.05f) && close_to(depth.width, 512, 0.1f), "depth keeps its full width");
    require(depth.y > 25.4f && depth.y < 25.8f && depth.y + depth.height > 398.1f && depth.y + depth.height < 398.5f &&
                depth.height > 372 && depth.height < 374,
            "depth crop y about 25.7..398.3 (about 373 rows)");
    // Not 4:3: both crops have the intersection's own shape, about 1.373:1.
    const float aspect = 1.373f;
    require(close_to(colour.width / colour.height, aspect, 2e-3f) && close_to(depth.width / depth.height, aspect, 2e-3f),
            "both crops are about 1.373:1");
    // Both crops look at the same tangent-space box (same angles).
    require(close_to(colour.width / 1060.0f, depth.width / 366.0f, 1e-4f) &&
                close_to(colour.height / 1060.0f, depth.height / 366.0f, 1e-4f), "the same view");
    // Original: the whole image.
    const sfr::KinectV2Crop whole = sfr::kinect_v2_crop(Kind::colour, View::original);
    require(whole.x == 0 && whole.y == 0 && whole.width == 1920 && whole.height == 1080, "original colour is whole");
    const sfr::KinectV2Crop whole_depth = sfr::kinect_v2_crop(Kind::depth_and_player, View::original);
    require(whole_depth.x == 0 && whole_depth.y == 0 && whole_depth.width == 512 && whole_depth.height == 424,
            "original depth is whole");
}

void preview_images_follow_the_pictures_shape() {
    const sfr::KinectImageSize colour = sfr::kinect_v2_preview_size(Kind::colour, View::common);
    const sfr::KinectImageSize depth = sfr::kinect_v2_preview_size(Kind::depth_and_player, View::common);
    require(colour.width == 640 && colour.height == 466, "common colour 640x466");
    require(depth.width == 512 && depth.height == 373, "common depth 512x373");
    const float colour_aspect = float(colour.width) / float(colour.height);
    const float depth_aspect = float(depth.width) / float(depth.height);
    require(close_to(colour_aspect, 1.373f, 2e-3f) && close_to(depth_aspect, 1.373f, 2e-3f), "both images about 1.373:1");
    const sfr::KinectImageSize colour_whole = sfr::kinect_v2_preview_size(Kind::colour, View::original);
    const sfr::KinectImageSize depth_whole = sfr::kinect_v2_preview_size(Kind::depth_and_player, View::original);
    require(colour_whole.width == 640 && colour_whole.height == 360, "original colour 640x360 (16:9)");
    require(depth_whole.width == 512 && depth_whole.height == 424, "original depth 512x424");
    require(close_to(float(colour_whole.width) / float(colour_whole.height), 16.0f / 9.0f, 1e-3f), "colour 16:9");
    require(close_to(float(depth_whole.width) / float(depth_whole.height), 512.0f / 424.0f, 1e-3f), "depth 512:424");
}

void pictures_cover_their_whole_image() {
    // The image is the crop, so the picture is all of it, in either view.
    for (const View view : {View::common, View::original})
        for (const Kind kind : {Kind::colour, Kind::depth_and_player}) {
            const sfr::KinectImageSize size = sfr::kinect_v2_preview_size(kind, view);
            const sfr::KinectImageRect rect = sfr::kinect_v2_picture_rect(kind, view);
            require(rect.x == 0 && rect.y == 0 && close_to(rect.width, float(size.width)) &&
                        close_to(rect.height, float(size.height)), "the rectangle is the whole image");
            // The crop's corners land (to rounding) on the image's corners.
            const bool is_colour = kind == Kind::colour;
            const sfr::KinectV2Crop crop = sfr::kinect_v2_crop(kind, view);
            const auto to_preview = [&](float x, float y) {
                return is_colour ? sfr::kinect_v2_colour_to_preview(x, y, view) : sfr::kinect_v2_depth_to_preview(x, y, view);
            };
            const sfr::KinectImagePoint top_left = to_preview(crop.x, crop.y);
            const sfr::KinectImagePoint bottom_right = to_preview(crop.x + crop.width, crop.y + crop.height);
            require(close_to(top_left.x, 0, 0.6f) && close_to(top_left.y, 0, 0.6f) &&
                        close_to(bottom_right.x, rect.width, 0.6f) && close_to(bottom_right.y, rect.height, 0.6f),
                    "the crop fills the image");
            require(rect.contains(rect.width / 2, rect.height / 2) && rect.contains(0, 0) && rect.contains(1, 1) &&
                        !rect.contains(rect.width, rect.height / 2) && !rect.contains(-1, 1), "no bars");
        }
}

void yuy2_converts_as_bt601() {
    require(sfr::kinect_yuy2_bgrx(16, 128, 128) == 0x000000u, "video black");
    require(sfr::kinect_yuy2_bgrx(235, 128, 128) == 0xFFFFFFu, "video white");
    // Pure red in BT.601 video range is about Y 81, U 90, V 240.
    const uint32_t red = sfr::kinect_yuy2_bgrx(81, 90, 240);
    require((red >> 16 & 0xFF) > 240 && (red >> 8 & 0xFF) < 10 && (red & 0xFF) < 10, "red");
}

std::vector<uint8_t> blue_red_frame(uint32_t w, uint32_t h) {
    std::vector<uint8_t> bgra(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &bgra[(size_t(y) * w + x) * 4];
            p[0] = x < w / 2 ? 255 : 0;
            p[2] = x < w / 2 ? 0 : 255;
            p[3] = 255;
        }
    return bgra;
}

void colour_original_is_the_whole_16_by_9_and_averaged() {
    // A BGRA frame whose left half is blue and right half is red, with one
    // green pixel in the block the target pixel (0, 0) averages.
    const uint32_t w = 1920, h = 1080;
    std::vector<uint8_t> bgra = blue_red_frame(w, h);
    bgra[1] = 255;
    sfr::KinectImage image;
    image.number = 7;
    sfr::kinect_v2_colour_from_bgra(bgra.data(), w, h, View::original, image);
    require(image.number == 7, "the number is the caller's");
    require(image.width == 640 && image.height == 360 && image.bytes_per_pixel == 4, "16:9, a third the size");
    require(image.pixels.size() == size_t(640) * 360 * 4, "pixels");
    require(colour_at(image, 10, 0) == 0x0000FFu && colour_at(image, 10, 359) == 0x0000FFu, "no bars above or below");
    require(colour_at(image, 630, 359) == 0xFF0000u, "red on the right");
    // 255 / 9 rounds to 28: the 3x3 block is averaged, not sampled.
    require((colour_at(image, 0, 0) >> 8 & 0xFF) == 28, "a 3x3 block averaged");
}

void colour_common_is_the_crop_without_bars() {
    const uint32_t w = 1920, h = 1080;
    std::vector<uint8_t> bgra = blue_red_frame(w, h);
    bgra[1] = 255;  // outside the crop (x < 219): must not show
    sfr::KinectImage image;
    sfr::kinect_v2_colour_from_bgra(bgra.data(), w, h, View::common, image);
    require(image.width == 640 && image.height == 466, "the crop's shape");
    require(colour_at(image, 10, 0) == 0x0000FFu && colour_at(image, 10, 465) == 0x0000FFu, "blue to the top and bottom edge");
    require(colour_at(image, 630, 0) == 0xFF0000u && colour_at(image, 630, 465) == 0xFF0000u, "red to the top and bottom edge");
    require(colour_at(image, 0, 0) == 0x0000FFu, "no bar, no outside pixel");
    // The picture's middle (source x 960) is the image's middle.
    require(colour_at(image, 317, 233) == 0x0000FFu && colour_at(image, 323, 233) == 0xFF0000u, "the middle stays the middle");
}

void yuy2_frames_use_each_pixels_own_luma() {
    // Pixel pairs sharing one U and V: the first pair black then white,
    // the others white; each pixel takes its own Y. Small enough to be
    // scaled up, so each target pixel shows one source pixel.
    const uint32_t w = 6, h = 3;
    std::vector<uint8_t> yuy2(size_t(w) * h * 2);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t pair = 0; pair < w / 2; ++pair) {
            uint8_t* p = &yuy2[(size_t(y) * w + pair * 2) * 2];
            p[0] = pair == 0 ? 16 : 235;  // Y0
            p[1] = 128;                   // U
            p[2] = 235;                   // Y1
            p[3] = 128;                   // V
        }
    sfr::KinectImage image;
    sfr::kinect_v2_colour_from_yuy2(yuy2.data(), w, h, View::original, image);
    require(image.width == 640 && image.height == 360, "always the preview size");
    // 6x3 in 640x360: scaled up, 640 wide and 320 tall, bars of 20.
    require(colour_at(image, 0, 19) == 0, "bar above");
    require(colour_at(image, 0, 20) == 0x000000u, "the first pixel is black");
    require(colour_at(image, 639, 20) == 0xFFFFFFu, "the last pixel is white");
    require(colour_at(image, 150, 200) == 0xFFFFFFu, "the second pixel, sharing U and V, is white");
}

void depth_carries_millimetres_and_bodies() {
    const uint32_t w = 512, h = 424;
    std::vector<uint16_t> depth(size_t(w) * h, 2000);
    std::vector<uint8_t> bodies(size_t(w) * h, sfr::kinect_v2_no_body);
    // Body 2 in the middle; a far reading beyond 13 bits at the top left.
    for (uint32_t y = 200; y < 224; ++y)
        for (uint32_t x = 240; x < 272; ++x) bodies[size_t(y) * w + x] = 2;
    depth[0] = 9000;
    sfr::KinectImage image;
    image.number = 3;
    sfr::kinect_v2_depth_image(depth.data(), bodies.data(), w, h, View::original, image);
    require(image.number == 3, "the number is the caller's");
    require(image.width == 512 && image.height == 424 && image.bytes_per_pixel == 2, "the whole depth image");
    require(depth_at(image, 0, 120) == uint16_t(2000 << 3) && depth_at(image, 511, 120) == uint16_t(2000 << 3), "no bars at the sides");
    require(depth_at(image, 100, 50) == uint16_t(2000 << 3), "millimetres shifted up three, no player");
    require(depth_at(image, 256, 212) == uint16_t(2000 << 3 | 3), "body 2 is player 3");
    require(depth_at(image, 0, 0) == uint16_t(0x1FFF << 3), "far readings clamp to 13 bits");
    // Without the body index, no player at all.
    sfr::kinect_v2_depth_image(depth.data(), nullptr, w, h, View::original, image);
    require(depth_at(image, 256, 212) == uint16_t(2000 << 3), "no body index, no player");
    // The common view: full width, about 26 rows less above and below, the
    // top-left reading (outside the crop) gone.
    sfr::kinect_v2_depth_image(depth.data(), bodies.data(), w, h, View::common, image);
    require(image.width == 512 && image.height == 373, "the crop's shape");
    require(depth_at(image, 0, 186) == uint16_t(2000 << 3) && depth_at(image, 511, 186) == uint16_t(2000 << 3),
            "full width, no bars at the sides");
    require(depth_at(image, 256, 0) == uint16_t(2000 << 3) && depth_at(image, 256, 372) == uint16_t(2000 << 3), "to the top and bottom");
    require(depth_at(image, 0, 0) == uint16_t(2000 << 3), "the far reading is outside the crop");
    require(depth_at(image, 256, 186) == uint16_t(2000 << 3 | 3), "body 2 is still player 3, in the middle");
}

void points_move_with_the_images() {
    // The principal points are the images' centres, in either view.
    for (const View view : {View::common, View::original}) {
        const sfr::KinectImageSize depth_size = sfr::kinect_v2_preview_size(Kind::depth_and_player, view);
        const sfr::KinectImagePoint centre = sfr::kinect_v2_depth_to_preview(256, 212, view);
        require(centre.visible && close_to(centre.x, depth_size.width / 2.0f, 0.01f) &&
                    close_to(centre.y, depth_size.height / 2.0f, 0.01f), "depth centre");
        const sfr::KinectImageSize colour_size = sfr::kinect_v2_preview_size(Kind::colour, view);
        const sfr::KinectImagePoint colour_centre = sfr::kinect_v2_colour_to_preview(960, 540, view);
        require(colour_centre.visible && close_to(colour_centre.x, colour_size.width / 2.0f, 0.01f) &&
                    close_to(colour_centre.y, colour_size.height / 2.0f, 0.01f), "colour centre");
    }
    // Original: the image's corners are the picture's corners.
    const sfr::KinectImagePoint corner = sfr::kinect_v2_depth_to_preview(0, 0, View::original);
    require(corner.visible && close_to(corner.x, 0, 0.01f) && close_to(corner.y, 0, 0.01f), "depth corner");
    const sfr::KinectImagePoint depth_far = sfr::kinect_v2_depth_to_preview(512, 424, View::original);
    require(close_to(depth_far.x, 512, 0.01f) && close_to(depth_far.y, 424, 0.01f), "depth far corner");
    const sfr::KinectImagePoint colour = sfr::kinect_v2_colour_to_preview(1920, 1080, View::original);
    require(colour.visible && close_to(colour.x, 640) && close_to(colour.y, 360), "colour corner");
    // Common: the image of the crop, so the image's own corner lies outside.
    const sfr::KinectImagePoint image_corner = sfr::kinect_v2_colour_to_preview(0, 0, View::common);
    require(image_corner.visible && image_corner.x < 0, "the image's own corner is outside the cropped panel (placed, then clipped)");
    const sfr::KinectImagePoint depth_top = sfr::kinect_v2_depth_to_preview(256, 0, View::common);
    require(depth_top.visible && depth_top.y < 0, "the depth rows above the crop are outside");
    // The mapper's answer for a point it cannot place.
    require(!sfr::kinect_v2_depth_to_preview(-INFINITY, -INFINITY, View::common).visible, "infinite is not visible");
    require(!sfr::kinect_v2_colour_to_preview(NAN, 3, View::original).visible, "NaN is not visible");
    require(!sfr::kinect_v2_colour_to_preview(1e7f, 3, View::common).visible, "far outside is not drawn");
    // Visible means placed, not inside the image: a point just beyond the
    // edge stays placed (and so keeps its bone), for the window to clip.
    const sfr::KinectImagePoint beyond = sfr::kinect_v2_colour_to_preview(-100, 540, View::original);
    require(beyond.visible && beyond.x < 0, "just outside the image is still placed");
    const sfr::KinectImagePoint beyond_depth = sfr::kinect_v2_depth_to_preview(600, 212, View::original);
    require(beyond_depth.visible && beyond_depth.x > 512, "beyond the depth image's right edge is still placed");
}

void switching_the_view_changes_pictures_and_points_together() {
    // A point near the cropped edge: inside the picture in one view, and
    // the picture rectangles differ; each view's point lies in its own
    // rectangle exactly when it is in that view's crop.
    const sfr::KinectImagePoint in_common = sfr::kinect_v2_colour_to_preview(300, 540, View::common);
    const sfr::KinectImagePoint in_original = sfr::kinect_v2_colour_to_preview(300, 540, View::original);
    const sfr::KinectImageRect common_rect = sfr::kinect_v2_picture_rect(Kind::colour, View::common);
    const sfr::KinectImageRect original_rect = sfr::kinect_v2_picture_rect(Kind::colour, View::original);
    require(common_rect.contains(in_common.x, in_common.y) && original_rect.contains(in_original.x, in_original.y),
            "inside the crop is inside the picture, either view");
    const sfr::KinectImagePoint cut_common = sfr::kinect_v2_colour_to_preview(100, 540, View::common);
    const sfr::KinectImagePoint cut_original = sfr::kinect_v2_colour_to_preview(100, 540, View::original);
    require(!common_rect.contains(cut_common.x, cut_common.y) && original_rect.contains(cut_original.x, cut_original.y),
            "left of the crop: outside the cropped picture, inside the whole one");
    require(common_rect.height > original_rect.height, "the cropped picture is taller");
    require(in_common.x != in_original.x, "the same sensor point lands differently");
    // The lens fallback lands in the same rectangles.
    const sfr::KinectImagePoint ahead_common = sfr::kinect_v2_colour_estimate({0, 0, 2}, View::common);
    const sfr::KinectImagePoint ahead_original = sfr::kinect_v2_colour_estimate({0, 0, 2}, View::original);
    require(close_to(ahead_common.x, 320, 0.01f) && close_to(ahead_common.y, 233, 0.01f) &&
                close_to(ahead_original.x, 320, 0.01f) && close_to(ahead_original.y, 180, 0.01f), "straight ahead is the middle");
}

void estimates_follow_the_lens() {
    // Straight ahead is the image's centre; a metre right at two metres is
    // 183 depth pixels right (366 / 2) before fitting.
    const sfr::KinectImagePoint ahead = sfr::kinect_v2_depth_estimate({0, 0, 2}, View::common);
    require(ahead.visible && close_to(ahead.x, 256, 0.01f) && close_to(ahead.y, 186.5f, 0.01f), "ahead is the centre");
    // Original: 1:1 with the depth image.
    const sfr::KinectImagePoint right = sfr::kinect_v2_depth_estimate({1, 0.5f, 2}, View::original);
    require(close_to(right.x, 256 + 183, 0.01f) && close_to(right.y, 212 - 91.5f, 0.01f), "the lens");
    // Common: the crop's own scale (image pixels per depth pixel, about 1).
    const sfr::KinectV2Crop crop = sfr::kinect_v2_crop(Kind::depth_and_player, View::common);
    const sfr::KinectImageRect rect = sfr::kinect_v2_picture_rect(Kind::depth_and_player, View::common);
    const float common_scale = rect.width / crop.width;
    const sfr::KinectImagePoint right_common = sfr::kinect_v2_depth_estimate({1, 0.5f, 2}, View::common);
    require(close_to(right_common.x, 256 + 183 * common_scale, 0.01f) &&
                close_to(right_common.y, rect.height / 2 - 91.5f * common_scale, 0.01f), "the lens, cropped");
    const sfr::KinectImagePoint up = sfr::kinect_v2_colour_estimate({0, 0.5f, 2.5f}, View::original);
    require(close_to(up.x, 320, 0.01f) && close_to(up.y, (540 - 212.0f) / 3.0f, 0.01f), "the colour lens");
    require(!sfr::kinect_v2_depth_estimate({0, 0, 0}, View::common).visible, "no depth, not visible");
    require(!sfr::kinect_v2_colour_estimate({0, 0, 0.05f}, View::original).visible, "too near, not visible");
}

void the_mapper_comes_first() {
    // A point the mapper placed goes where it says, not where the lens would.
    const std::array<float, 3> p{0.3f, 0.2f, 2.0f};
    for (const View view : {View::common, View::original}) {
        const float depth_pixel[2] = {100, 50};
        const sfr::KinectImagePoint depth = sfr::kinect_v2_place(Kind::depth_and_player, p, depth_pixel, view);
        const sfr::KinectImagePoint depth_expected = sfr::kinect_v2_depth_to_preview(100, 50, view);
        require(depth.visible && close_to(depth.x, depth_expected.x) && close_to(depth.y, depth_expected.y),
                "a mapped depth point is the mapper's");
        const float colour_pixel[2] = {1500, 300};
        const sfr::KinectImagePoint colour = sfr::kinect_v2_place(Kind::colour, p, colour_pixel, view);
        const sfr::KinectImagePoint colour_expected = sfr::kinect_v2_colour_to_preview(1500, 300, view);
        require(colour.visible && close_to(colour.x, colour_expected.x) && close_to(colour.y, colour_expected.y),
                "a mapped colour point is the mapper's");
        // -infinity (the mapper could not place it) or no mapper: the lens.
        const float unplaced[2] = {-INFINITY, -INFINITY};
        const sfr::KinectImagePoint lens = sfr::kinect_v2_depth_estimate(p, view);
        const sfr::KinectImagePoint fallback = sfr::kinect_v2_place(Kind::depth_and_player, p, unplaced, view);
        require(fallback.visible && close_to(fallback.x, lens.x) && close_to(fallback.y, lens.y), "-inf falls back to the lens");
        const float half[2] = {700, -INFINITY};
        const sfr::KinectImagePoint colour_lens = sfr::kinect_v2_colour_estimate(p, view);
        const sfr::KinectImagePoint half_fallback = sfr::kinect_v2_place(Kind::colour, p, half, view);
        require(close_to(half_fallback.x, colour_lens.x) && close_to(half_fallback.y, colour_lens.y),
                "one infinite coordinate falls back too");
        const sfr::KinectImagePoint none = sfr::kinect_v2_place(Kind::colour, p, nullptr, view);
        require(close_to(none.x, colour_lens.x) && close_to(none.y, colour_lens.y), "no mapper, the lens");
        // Behind the sensor neither can place it.
        require(!sfr::kinect_v2_place(Kind::depth_and_player, {0, 0, 0}, unplaced, view).visible, "nowhere, not visible");
    }
    // Original colour: a third of the sensor's pixels.
    const float colour_pixel[2] = {1500, 300};
    const sfr::KinectImagePoint old = sfr::kinect_v2_place(Kind::colour, {0.3f, 0.2f, 2.0f}, colour_pixel, View::original);
    require(close_to(old.x, 500) && close_to(old.y, 100), "original colour is the 16:9 image, a third the size");
}
}

int main() {
    try {
        fits_keep_the_shape();
        crops_are_the_common_view();
        preview_images_follow_the_pictures_shape();
        pictures_cover_their_whole_image();
        yuy2_converts_as_bt601();
        colour_original_is_the_whole_16_by_9_and_averaged();
        colour_common_is_the_crop_without_bars();
        yuy2_frames_use_each_pixels_own_luma();
        depth_carries_millimetres_and_bodies();
        points_move_with_the_images();
        switching_the_view_changes_pictures_and_points_together();
        estimates_follow_the_lens();
        the_mapper_comes_first();
    } catch (const std::exception& error) {
        std::cerr << "kinect_v2_image_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "kinect_v2_image_test passed\n";
    return 0;
}
