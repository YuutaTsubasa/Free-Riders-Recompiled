#include "kinect_preview_layout.h"

#include <algorithm>
#include <cmath>

namespace sfr {

KinectPreviewLayout kinect_preview_layout(const KinectPreviewLayoutInput& input) {
    KinectPreviewLayout layout;
    const double colour = input.colour_aspect > 0 ? input.colour_aspect : 4.0 / 3.0;
    const double depth = input.same_size ? colour : (input.depth_aspect > 0 ? input.depth_aspect : 4.0 / 3.0);
    const int available_width = input.client_width - 2 * input.gap - input.gap;  // both sides and between
    const int available_height = input.client_height - input.top - input.bottom;
    if (available_width <= 0 || available_height <= 0) return layout;
    // One height for both; the widths follow from it.
    const double height = std::min(double(available_height), double(available_width) / (colour + depth));
    const int panel_height = int(std::floor(height));
    int colour_width = int(std::floor(panel_height * colour + 1e-6));
    int depth_width = input.same_size ? colour_width : int(std::floor(panel_height * depth + 1e-6));
    // Rounding must never push the pair out of the area (the epsilon above
    // could, at an exact fit, by a pixel).
    if (colour_width + depth_width > available_width) {
        if (input.same_size) colour_width = depth_width = available_width / 2;
        else depth_width = available_width - colour_width;
    }
    if (panel_height <= 0 || colour_width <= 0 || depth_width <= 0) return layout;
    layout.colour = {input.gap, input.top, input.gap + colour_width, input.top + panel_height};
    layout.depth = {2 * input.gap + colour_width, input.top, 2 * input.gap + colour_width + depth_width,
                    input.top + panel_height};
    return layout;
}

KinectRecompiledLayout kinect_preview_recompiled_layout(const KinectRecompiledLayoutInput& input) {
    KinectRecompiledLayout layout;
    const auto aspect = [](double value, double fallback) { return value > 0 ? value : fallback; };
    const double v2_colour = aspect(input.v2_colour_aspect, 16.0 / 9.0), v2_depth = aspect(input.v2_depth_aspect, 512.0 / 424.0);
    const double v1_colour = aspect(input.v1_colour_aspect, 4.0 / 3.0), v1_depth = aspect(input.v1_depth_aspect, 4.0 / 3.0);
    // Two columns of one width; every picture is that wide (its height from
    // its aspect), at the top of its cell, so colour sits exactly under
    // colour and depth under depth, and a row is as tall as its taller one.
    const double row_top = std::max(1.0 / v2_colour, 1.0 / v2_depth), row_bottom = std::max(1.0 / v1_colour, 1.0 / v1_depth);
    const int available_width = input.client_width - 4 * input.gap;  // both sides, between the columns, before the diagram
    const int available_height = input.client_height - input.top - input.bottom;
    if (available_width <= 0 || available_height <= input.caption) return layout;
    // At most two thirds for the grid; as wide as the height allows.
    const int grid_most = available_width - available_width / 3;
    const double column = std::min(double(grid_most) / 2, double(available_height - input.caption) / (row_top + row_bottom));
    const int column_width = int(std::floor(column));
    if (column_width <= 0) return layout;
    const int top_height = int(std::floor(column_width * row_top + 1e-6)), bottom_height = int(std::floor(column_width * row_bottom + 1e-6));
    const int left = input.gap, second = 2 * input.gap + column_width;
    const int top = input.top, lower = input.top + top_height + input.caption;
    layout.colour_cells[0] = {left, top, left + column_width, top + top_height};
    layout.colour_cells[1] = {left, lower, left + column_width, lower + bottom_height};
    layout.depth_cells[0] = {second, top, second + column_width, top + top_height};
    layout.depth_cells[1] = {second, lower, second + column_width, lower + bottom_height};
    const auto at_top = [&](const KinectPanelRect& cell, double aspect) {
        const int height = std::min(cell.height(), int(std::floor(cell.width() / aspect + 1e-6)));
        return KinectPanelRect{cell.left, cell.top, cell.right, cell.top + height};
    };
    layout.v2_colour = at_top(layout.colour_cells[0], v2_colour);
    layout.v1_colour = at_top(layout.colour_cells[1], v1_colour);
    layout.v2_depth = at_top(layout.depth_cells[0], v2_depth);
    layout.v1_depth = at_top(layout.depth_cells[1], v1_depth);
    // The diagram takes the rest of the width (a third or more), the
    // grid's full height.
    const int diagram_left = second + column_width + input.gap;
    layout.diagram = {diagram_left, top, input.client_width - input.gap, lower + bottom_height};
    if (layout.diagram.width() <= 0) layout = {};
    return layout;
}

}
