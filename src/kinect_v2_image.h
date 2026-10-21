#pragma once
#include "kinect_sensor.h"

#include <array>
#include <cstdint>

namespace sfr {

// A Kinect v2's cameras as the preview window shows them. The preview was
// made for a Kinect v1's images (KinectImage): colour 640x480 and depth and
// player index 320x240, both 4:3. A v2's colour camera is 1920x1080 (16:9,
// about 84x54 degrees) and its depth camera 512x424 (about 71x60 degrees), so
// the two would show different parts of the room. The preview therefore shows
// only the view the cameras have in common (kinect_v2_crop), in images of
// that shape (kinect_v2_preview_size) that its panels follow, with no bars. This is the preview's own picture:
// the depth the game gets (kinect_v2_depth.h) is made from the whole image.
// Skeleton points are placed by the sensor's own coordinate mapper
// (ICoordinateMapper) and moved the same way (kinect_v2_place), with the
// typical lens (kinect_v2_*_estimate) when the mapper is not there or cannot
// place a point, so the preview places them as it does a v1's.
//
// None of this is Windows', so it is tested everywhere (kinect_v2_image_test).

constexpr uint32_t kinect_v2_colour_width = 1920, kinect_v2_colour_height = 1080;
constexpr uint32_t kinect_v2_depth_width = 512, kinect_v2_depth_height = 424;
// The v1 sizes the preview draws (a v2's follow its pictures' shape:
// kinect_v2_preview_size).
constexpr uint32_t kinect_preview_colour_width = 640, kinect_preview_colour_height = 480;
constexpr uint32_t kinect_preview_depth_width = 320, kinect_preview_depth_height = 240;
// A v2 body index pixel that is no body (BodyIndexFrame).
constexpr uint8_t kinect_v2_no_body = 255;

// Where a source image of one size lands in a target of another: scaled
// uniformly to fit, centred.
struct KinectImageSize {
    uint32_t width = 0, height = 0;
};
struct KinectFit {
    float scale = 0;              // target pixels per source pixel
    float offset_x = 0, offset_y = 0;  // where the source's (0, 0) lands
};
KinectFit kinect_fit(uint32_t source_width, uint32_t source_height, uint32_t target_width, uint32_t target_height);

// What the preview shows of each camera: the view both cameras have
// (common, the default) or each camera's whole picture (original: in its own
// shape, colour 640x360 and depth 512x424, no bars). A runtime choice
// of the preview window, passed to everything below; never the game's.
enum class KinectV2View { common, original };

// The part of a sensor image the preview shows, in that image's own pixels
// (colour 1920x1080, depth 512x424).
struct KinectV2Crop {
    float x = 0, y = 0, width = 0, height = 0;
};

// The typical lenses (pixels, and the optical centre) the crop is worked out
// from. The depth ones are a v2's usual (the sensor's own differ a few
// pixels). The colour ones are NOMINAL: there is no intrinsics API for the
// colour camera, and the cameras sit a few centimetres apart (about a degree
// at two metres), so the alignment of the two pictures is approximate.
constexpr float kinect_v2_depth_focal = 366.0f, kinect_v2_depth_centre_x = 256.0f, kinect_v2_depth_centre_y = 212.0f;
constexpr float kinect_v2_colour_focal = 1060.0f, kinect_v2_colour_centre_x = 960.0f, kinect_v2_colour_centre_y = 540.0f;

// The view both v2 cameras have (the intersection of their fields of view,
// about 1.373:1), in tangent space (x / z, y / z) about each camera's optical
// axis: half-height the lower of the two cameras' (colour 0.509, depth
// 0.579), half-width likewise (depth 0.699, colour 0.906). Colour is x
// 219..1701, y 0..1080; depth x 0..512, y 25.7..398.3; in the original view
// the whole image. The one place the crop is worked out: the pixel
// conversion, the point mapping and the picture rectangle all come from it.
KinectV2Crop kinect_v2_crop(KinectImageKind kind, KinectV2View view);

// The long side of a v2's preview image: colour 640, depth 512 (the depth
// camera's own width). The short side follows the crop's shape, so the image
// IS the crop with no bars: common colour 640x466, depth 512x373; original
// colour 640x360, depth 512x424.
constexpr uint32_t kinect_v2_colour_preview_long_side = 640, kinect_v2_depth_preview_long_side = 512;
KinectImageSize kinect_v2_preview_size(KinectImageKind kind, KinectV2View view);

// Where a crop of a source image lands in a target: scaled uniformly to fit,
// centred. The fit is for the source's own pixels (its (0, 0) lands where
// the source's corner would), so kinect_fit_point places a source position.
KinectFit kinect_fit_crop(const KinectV2Crop& crop, uint32_t target_width, uint32_t target_height);

// Where a v2's picture lies in the preview's image of one kind: all of it
// (the image is the crop, at kinect_v2_preview_size).
KinectImageRect kinect_v2_picture_rect(KinectImageKind kind, KinectV2View view);

// A source pixel position (the sensor's own image space) in the target.
KinectImagePoint kinect_fit_point(const KinectFit& fit, float x, float y, uint32_t target_width,
                                  uint32_t target_height);

// One YUY2 (Y0 U Y1 V) pixel pair's colour for one of its pixels, as B, G,
// R, 0 (ITU-R BT.601, video range: the format a v2's colour camera sends).
uint32_t kinect_yuy2_bgrx(uint8_t y, uint8_t u, uint8_t v);

// A colour image, its common view (kinect_v2_crop, scaled to the frame's
// size) fitted into kinect_v2_preview_size (KinectImage colour:
// B, G, R, unused) by averaging the source pixels each target pixel covers.
// The source is either YUY2 (2 bytes a pixel, pairs sharing U and V) or
// BGRA (4 bytes a pixel). The image's number is left alone.
void kinect_v2_colour_from_yuy2(const uint8_t* yuy2, uint32_t width, uint32_t height, KinectV2View view,
                                KinectImage& out);
void kinect_v2_colour_from_bgra(const uint8_t* bgra, uint32_t width, uint32_t height, KinectV2View view,
                                KinectImage& out);

// A depth image (millimetres, 0 for nothing measured) with each pixel's body
// (0..5, kinect_v2_no_body for none; null when unknown), fitted into the
// preview size as a v1's depth and player index: the millimetres
// shifted up three (clamped to the 13 bits there are), the body plus one in
// the low three bits. Nearest pixel, so that no depth is averaged across an
// edge. The image's number is left alone.
void kinect_v2_depth_image(const uint16_t* millimetres, const uint8_t* body_index, uint32_t width,
                           uint32_t height, KinectV2View view, KinectImage& out);

// Image positions in the sensor's own images (512x424 depth, 1920x1080
// colour) moved into the preview's images, as the coordinate mapper gives
// them. A point the mapper could not place (infinite) is not visible.
KinectImagePoint kinect_v2_depth_to_preview(float x, float y, KinectV2View view);
KinectImagePoint kinect_v2_colour_to_preview(float x, float y, KinectV2View view);

// A skeleton point (camera space, metres) in the preview's image of one
// kind. mapped is the coordinate mapper's answer for it in the sensor's own
// image (x, y; depth pixels for depth_and_player, colour pixels for colour),
// or null without one. The mapper answers -infinity for a point it cannot
// place; then, and without an answer, the typical lens places it instead
// (kinect_v2_*_estimate).
KinectImagePoint kinect_v2_place(KinectImageKind kind, const std::array<float, 3>& p, const float* mapped,
                                 KinectV2View view);

// A colour picture made pixel by pixel from another one: positions holds,
// for each of count output pixels, where it lands in the colour image
// (x, y pairs in its pixels, as KinectSensor::colour_image_positions gives
// them; NaN for none), which is width x height pixels of B, G, R, unused.
// The nearest pixel's colour, or black for none or outside the image.
void kinect_colour_from_positions(const float* positions, size_t count, const uint32_t* colour, uint32_t width,
                                  uint32_t height, std::vector<uint32_t>& out);

// A skeleton point (camera space, metres) through a v2
// camera's typical lens, in the preview's images. Close (the cameras' own
// calibration differs a few pixels from sensor to sensor), and the colour
// one ignores that the colour camera sits about 5 cm beside the depth
// camera, so it is only a fallback.
KinectImagePoint kinect_v2_depth_estimate(const std::array<float, 3>& p, KinectV2View view);
KinectImagePoint kinect_v2_colour_estimate(const std::array<float, 3>& p, KinectV2View view);

}
