#pragma once

namespace sfr {

// Where the Kinect preview window puts its two picture panels (colour on the
// left, depth on the right). Pure arithmetic, so it is tested everywhere
// (kinect_preview_layout_test); the window paints and places skeletons from
// the same rectangles.
struct KinectPanelRect {
    int left = 0, top = 0, right = 0, bottom = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
};

struct KinectPreviewLayoutInput {
    int client_width = 0, client_height = 0;
    // Each picture's width / height.
    double colour_aspect = 4.0 / 3.0, depth_aspect = 4.0 / 3.0;
    // Both panels exactly the same size, at the colour aspect (the common
    // view of a Kinect v2, whose two pictures have the same shape).
    bool same_size = false;
    // Space around the panels: above them (the text), below (the hint line),
    // and at the sides and between them.
    int top = 110, bottom = 40, gap = 20;
};

struct KinectPreviewLayout {
    KinectPanelRect colour, depth;
};

// The two panels side by side, aligned at their top, the same height, each
// as wide as its picture's aspect says, the pair as large as fits in the
// client area (scaled down uniformly, never stretched). Empty panels when
// nothing fits.
KinectPreviewLayout kinect_preview_layout(const KinectPreviewLayoutInput& input);

// The Recompiled view's five areas: on the left a two by two grid, the real
// v2's whole pictures on top (colour, depth) and the virtual v1's below
// (colour reprojected into the v1's view, the depth the game gets), colour
// above colour and depth above depth; on the right, one tall panel for the
// top-down diagram, the rest of the width (a third or more) and the grid's
// full height. Both columns are one width and every picture is that wide,
// keeping its aspect, at the top of its cell (a row is as tall as its taller
// picture), as large as the window's height and two thirds of its width
// allow. Every row has a caption line above it (caption pixels, the gap
// between the rows: a line of text and some room; the first row's is inside
// top). Empty rectangles when nothing fits.
struct KinectRecompiledLayoutInput {
    int client_width = 0, client_height = 0;
    double v2_colour_aspect = 16.0 / 9.0, v2_depth_aspect = 512.0 / 424.0;
    double v1_colour_aspect = 4.0 / 3.0, v1_depth_aspect = 4.0 / 3.0;
    int top = 110, bottom = 40, gap = 20, caption = 32;
};
struct KinectRecompiledLayout {
    KinectPanelRect v2_colour, v2_depth, v1_colour, v1_depth;  // the pictures
    KinectPanelRect colour_cells[2], depth_cells[2];            // their cells (top row first)
    KinectPanelRect diagram;
};
KinectRecompiledLayout kinect_preview_recompiled_layout(const KinectRecompiledLayoutInput& input);

}
