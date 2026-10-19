#include "native_depth_flip.h"

#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// depth = min + (max - min) * z for the device's viewport and the shader's z.
static float stored(const plume::RenderViewport& viewport, float z) {
    return viewport.minDepth + (viewport.maxDepth - viewport.minDepth) * z;
}

static void inverted_viewports_flip_only_without_device_support() {
    const plume::RenderViewport title(0, 0, 1280, 720, 1, 0);
    require(!sfr::depth_flip_needed(title, true), "a capable device draws the inverted viewport itself");
    require(sfr::depth_flip_viewport(title, true) == title, "a capable device gets the title's viewport unchanged");
    require(sfr::depth_flip_needed(title, false), "without support an inverted viewport flips");
    require(sfr::depth_flip_viewport(title, false) == plume::RenderViewport(0, 0, 1280, 720, 0, 1),
            "without support the range is put in order and nothing else changes");

    for (const auto& ordered : {plume::RenderViewport(0, 0, 640, 360, 0, 1), plume::RenderViewport(1, 2, 3, 4, 0.25f, 0.75f),
                                plume::RenderViewport(0, 0, 8, 8, 0.5f, 0.5f)}) {
        require(!sfr::depth_flip_needed(ordered, false), "an ordered or empty range never flips");
        require(sfr::depth_flip_viewport(ordered, false) == ordered, "an ordered or empty range is given as it is");
    }

    // The swapped range with z' = 1 - z (w - z over w) stores what the
    // inverted one stores, here for a partial inverted range too.
    for (const auto& inverted : {title, plume::RenderViewport(0, 0, 8, 8, 0.75f, 0.25f)}) {
        const auto device = sfr::depth_flip_viewport(inverted, false);
        for (const float z : {0.0f, 0.125f, 0.5f, 0.75f, 1.0f})
            require(stored(device, 1.0f - z) == stored(inverted, z), "flipped z stores the inverted viewport's depth");
    }
}

static void geometry_stage_follows_the_topology() {
    using T = plume::RenderPrimitiveTopology;
    using S = sfr::DepthFlipStage;
    require(sfr::depth_flip_stage(T::POINT_LIST) == S::point, "points");
    require(sfr::depth_flip_stage(T::LINE_LIST) == S::line && sfr::depth_flip_stage(T::LINE_STRIP) == S::line, "lines");
    require(sfr::depth_flip_stage(T::TRIANGLE_LIST) == S::triangle && sfr::depth_flip_stage(T::TRIANGLE_STRIP) == S::triangle &&
            sfr::depth_flip_stage(T::TRIANGLE_FAN) == S::triangle, "triangles");
    require(sfr::depth_flip_stage(T::UNKNOWN) == S::none, "no stage for an unknown topology");
}

static void test_switch_takes_exactly_one() {
    require(sfr::inverted_depth_disabled_for_test("1"), "1 forces the flipped path");
    for (const char* other : {static_cast<const char*>(nullptr), "", "0", "10", "yes", "true"})
        require(!sfr::inverted_depth_disabled_for_test(other), "anything else keeps the device's own");
}

int main() {
    try {
        inverted_viewports_flip_only_without_device_support();
        geometry_stage_follows_the_topology();
        test_switch_takes_exactly_one();
        std::cout << "Native depth flip checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
