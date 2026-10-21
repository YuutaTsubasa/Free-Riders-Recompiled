#include "kinect_preview_layout.h"
#include "kinect_v2_image.h"

#include <cmath>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using sfr::KinectPanelRect;

bool inside(const KinectPanelRect& r, int width, int height) {
    return r.left >= 0 && r.top >= 0 && r.right <= width && r.bottom <= height && r.width() > 0 && r.height() > 0;
}

double aspect(const KinectPanelRect& r) { return double(r.width()) / double(r.height()); }

// The checks every layout shares: inside the client area, below the text,
// side by side without overlap, aligned at the top, one height.
void sane(const sfr::KinectPreviewLayout& layout, int width, int height) {
    require(inside(layout.colour, width, height) && inside(layout.depth, width, height), "panels stay inside the client area");
    require(layout.colour.right <= layout.depth.left, "panels do not overlap");
    require(layout.colour.top == layout.depth.top && layout.colour.height() == layout.depth.height(), "same top, same height");
    const sfr::KinectPreviewLayoutInput defaults;
    require(layout.colour.top >= defaults.top && layout.colour.bottom <= height - defaults.bottom, "clear of the text and the hint line");
}

void common_view_panels_are_the_same_size_and_shape() {
    const double a = double(sfr::kinect_v2_preview_size(sfr::KinectImageKind::colour, sfr::KinectV2View::common).width) /
                     sfr::kinect_v2_preview_size(sfr::KinectImageKind::colour, sfr::KinectV2View::common).height;
    require(std::fabs(a - 1.373) < 2e-3, "the common aspect");
    const int sizes[][2] = {{1360, 640}, {1920, 1080}, {744, 380}, {1000, 900}, {2560, 700}, {1600, 1200}};
    for (const auto& size : sizes) {
        sfr::KinectPreviewLayoutInput input;
        input.client_width = size[0];
        input.client_height = size[1];
        input.colour_aspect = a;
        input.depth_aspect = 99;  // ignored: same size
        input.same_size = true;
        const sfr::KinectPreviewLayout layout = sfr::kinect_preview_layout(input);
        sane(layout, size[0], size[1]);
        require(layout.colour.width() == layout.depth.width(), "same width");
        require(std::fabs(aspect(layout.colour) - a) < 0.02 && std::fabs(aspect(layout.depth) - a) < 0.02, "aspect kept");
    }
}

void original_view_panels_follow_their_own_pictures() {
    const int sizes[][2] = {{1360, 640}, {1920, 1080}, {744, 380}, {1000, 900}, {2560, 700}};
    for (const auto& size : sizes) {
        sfr::KinectPreviewLayoutInput input;
        input.client_width = size[0];
        input.client_height = size[1];
        input.colour_aspect = 16.0 / 9.0;
        input.depth_aspect = 512.0 / 424.0;
        const sfr::KinectPreviewLayout layout = sfr::kinect_preview_layout(input);
        sane(layout, size[0], size[1]);
        require(std::fabs(aspect(layout.colour) - 16.0 / 9.0) < 0.05, "colour 16:9");
        require(std::fabs(aspect(layout.depth) - 512.0 / 424.0) < 0.03, "depth 512:424");
    }
}

void panels_shrink_uniformly_and_use_the_space() {
    sfr::KinectPreviewLayoutInput input;
    input.same_size = true;
    input.colour_aspect = 1.373;
    // Wide window: the height limits (client height - text - hint).
    input.client_width = 2560;
    input.client_height = 700;
    sfr::KinectPreviewLayout layout = sfr::kinect_preview_layout(input);
    require(layout.colour.height() == 700 - input.top - input.bottom, "the height limits a wide window");
    // Tall window: the width limits, and a narrower window gets smaller panels.
    input.client_width = 1000;
    input.client_height = 1200;
    const sfr::KinectPreviewLayout narrow = sfr::kinect_preview_layout(input);
    require(narrow.depth.right <= 1000 - input.gap && narrow.depth.right > 1000 - input.gap - 3, "the width is used");
    input.client_width = 1360;
    const sfr::KinectPreviewLayout wider = sfr::kinect_preview_layout(input);
    require(wider.colour.width() > narrow.colour.width() && wider.colour.height() > narrow.colour.height(), "bigger window, bigger panels");
    // Nothing fits: empty panels, not negative ones.
    input.client_width = 30;
    input.client_height = 100;
    layout = sfr::kinect_preview_layout(input);
    require(layout.colour.width() == 0 && layout.depth.width() == 0 && layout.colour.height() == 0, "no room, no panels");
}

void principal_points_land_on_the_panels_centres() {
    // The window scales a preview image to its panel by the ratio of their
    // sizes; the principal point (the lens axis) must end at the panel's centre.
    using sfr::KinectImageKind;
    using sfr::KinectV2View;
    for (const KinectV2View view : {KinectV2View::common, KinectV2View::original}) {
        const sfr::KinectImageRect colour_image = sfr::kinect_v2_picture_rect(KinectImageKind::colour, view);
        const sfr::KinectImageRect depth_image = sfr::kinect_v2_picture_rect(KinectImageKind::depth_and_player, view);
        sfr::KinectPreviewLayoutInput input;
        input.client_width = 1360;
        input.client_height = 640;
        input.colour_aspect = colour_image.width / colour_image.height;
        input.depth_aspect = depth_image.width / depth_image.height;
        input.same_size = view == KinectV2View::common;
        const sfr::KinectPreviewLayout layout = sfr::kinect_preview_layout(input);
        sane(layout, 1360, 640);
        const sfr::KinectImagePoint colour = sfr::kinect_v2_colour_to_preview(960, 540, view);
        const sfr::KinectImagePoint depth = sfr::kinect_v2_depth_to_preview(256, 212, view);
        const double colour_x = layout.colour.left + colour.x * layout.colour.width() / colour_image.width;
        const double colour_y = layout.colour.top + colour.y * layout.colour.height() / colour_image.height;
        const double depth_x = layout.depth.left + depth.x * layout.depth.width() / depth_image.width;
        const double depth_y = layout.depth.top + depth.y * layout.depth.height() / depth_image.height;
        require(std::fabs(colour_x - (layout.colour.left + layout.colour.right) / 2.0) < 1.5 &&
                    std::fabs(colour_y - (layout.colour.top + layout.colour.bottom) / 2.0) < 1.5, "colour axis at the panel centre");
        require(std::fabs(depth_x - (layout.depth.left + layout.depth.right) / 2.0) < 1.5 &&
                    std::fabs(depth_y - (layout.depth.top + layout.depth.bottom) / 2.0) < 1.5, "depth axis at the panel centre");
    }
}

void a_v1_keeps_its_4_by_3_panels() {
    sfr::KinectPreviewLayoutInput input;  // the defaults are 4:3
    input.client_width = 1360;
    input.client_height = 640;
    const sfr::KinectPreviewLayout layout = sfr::kinect_preview_layout(input);
    sane(layout, 1360, 640);
    require(std::fabs(aspect(layout.colour) - 4.0 / 3.0) < 0.01 && std::fabs(aspect(layout.depth) - 4.0 / 3.0) < 0.01, "4:3");
    require(layout.colour.width() == layout.depth.width(), "equal");
}

bool within(const KinectPanelRect& inner, const KinectPanelRect& outer) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool apart(const KinectPanelRect& a, const KinectPanelRect& b) {
    return a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top;
}

void recompiled_grid_and_diagram() {
    for (const auto& [width, height] : {std::pair{1360, 820}, std::pair{1360, 640}, std::pair{760, 420}, std::pair{2400, 700}}) {
        sfr::KinectRecompiledLayoutInput input;
        input.client_width = width;
        input.client_height = height;
        const auto layout = sfr::kinect_preview_recompiled_layout(input);
        const KinectPanelRect pictures[] = {layout.v2_colour, layout.v2_depth, layout.v1_colour, layout.v1_depth, layout.diagram};
        for (const auto& picture : pictures) require(inside(picture, width, height), "every area inside the window");
        for (size_t i = 0; i < 5; ++i)
            for (size_t j = i + 1; j < 5; ++j) require(apart(pictures[i], pictures[j]), "no two areas overlap");
        require(within(layout.v2_colour, layout.colour_cells[0]) && within(layout.v1_colour, layout.colour_cells[1]) &&
                    within(layout.v2_depth, layout.depth_cells[0]) && within(layout.v1_depth, layout.depth_cells[1]),
                "each picture in its cell");
        require(std::fabs(aspect(layout.v2_colour) - 16.0 / 9.0) < 0.02 && std::fabs(aspect(layout.v2_depth) - 512.0 / 424.0) < 0.02 &&
                    std::fabs(aspect(layout.v1_colour) - 4.0 / 3.0) < 0.02 && std::fabs(aspect(layout.v1_depth) - 4.0 / 3.0) < 0.02,
                "true aspects");
        require(layout.colour_cells[0].left == layout.colour_cells[1].left && layout.depth_cells[0].left == layout.depth_cells[1].left &&
                    layout.colour_cells[0].right <= layout.depth_cells[0].left,
                "colour above colour, depth above depth");
        require(layout.colour_cells[1].top - layout.colour_cells[0].bottom >= input.caption && layout.v2_colour.top >= input.top &&
                    layout.v1_colour.top - layout.v2_depth.bottom >= 30 && layout.v1_depth.top - layout.v2_depth.bottom >= 30,
                "a caption line, and room above it, over each row");
        require(layout.diagram.left >= layout.depth_cells[0].right && layout.diagram.top == input.top &&
                    layout.diagram.bottom == layout.colour_cells[1].bottom && layout.diagram.right == width - input.gap &&
                    layout.diagram.width() >= (width - 4 * input.gap) / 3,
                "the diagram: a third of the width or more, the grid's full height, on the right");
        // Every picture as wide as its column, at the top of its cell.
        for (const auto& [picture, cell] : {std::pair{layout.v2_colour, layout.colour_cells[0]}, std::pair{layout.v1_colour, layout.colour_cells[1]},
                                            std::pair{layout.v2_depth, layout.depth_cells[0]}, std::pair{layout.v1_depth, layout.depth_cells[1]}})
            require(picture.left == cell.left && picture.right == cell.right && picture.top == cell.top, "the column's width, at the top");
        require(layout.v2_colour.width() == layout.v2_depth.width() && layout.v1_colour.width() == layout.v2_colour.width(),
                "one width for both columns");
        // The height is used: limited by it, or by two thirds of the width.
        const bool tall = layout.v1_depth.bottom >= height - input.bottom - 2;
        const int grid_most = (width - 4 * input.gap) - (width - 4 * input.gap) / 3;
        const bool wide = layout.depth_cells[0].right >= 2 * input.gap + 2 * (grid_most / 2) - 2;
        require(tall || wide, "as large as the window allows");
    }
    sfr::KinectRecompiledLayoutInput tiny;
    tiny.client_width = 60;
    tiny.client_height = 100;
    require(sfr::kinect_preview_recompiled_layout(tiny).diagram.width() == 0, "nothing fits: nothing");
}
}

int main() {
    try {
        common_view_panels_are_the_same_size_and_shape();
        original_view_panels_follow_their_own_pictures();
        panels_shrink_uniformly_and_use_the_space();
        principal_points_land_on_the_panels_centres();
        a_v1_keeps_its_4_by_3_panels();
        recompiled_grid_and_diagram();
    } catch (const std::exception& error) {
        std::cerr << "kinect_preview_layout_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "kinect_preview_layout_test passed\n";
    return 0;
}
