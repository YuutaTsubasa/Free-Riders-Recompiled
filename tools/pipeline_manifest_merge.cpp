// sfr_pipeline_manifest_merge <d3d12|vulkan> <output> <input>...
//
// Joins recorded pipeline lists (data/pipeline-manifests, or a player's
// pipeline-cache/<backend>.manifest) into one, through the runtime's own
// decoder and encoder: every input is validated as the game validates it,
// duplicates are dropped and the first-seen order is kept, so the first input
// keeps its recipes where they were.
#include "pipeline_manifest.h"

#include <cstdio>
#include <exception>
#include <set>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <d3d12|vulkan> <output> <input>...\n", argv[0]);
        return 2;
    }
    const std::string_view name = argv[1];
    if (name != "d3d12" && name != "vulkan") {
        std::fprintf(stderr, "backend must be d3d12 or vulkan\n");
        return 2;
    }
    const uint32_t backend = name == "vulkan" ? 1u : 0u;
    try {
        // Duplicates are dropped while reading: the encoder takes at most
        // pipeline_manifest_max_recipes, before it removes them itself.
        std::vector<sfr::PipelineRecipe> all;
        std::set<std::vector<uint8_t>> seen;
        size_t read = 0;
        for (int i = 3; i < argc; ++i) {
            const auto recipes = sfr::load_pipeline_manifest_file(argv[i], backend);
            std::printf("%s: %zu recipes\n", argv[i], recipes.size());
            read += recipes.size();
            for (const auto& recipe : recipes)
                if (seen.insert(sfr::pipeline_recipe_key(recipe)).second) all.push_back(recipe);
        }
        if (!sfr::save_pipeline_manifest_file(argv[2], all, backend)) {
            std::fprintf(stderr, "could not write %s\n", argv[2]);
            return 1;
        }
        const auto merged = sfr::load_pipeline_manifest_file(argv[2], backend);
        std::printf("%s: %zu recipes (from %zu read)\n", argv[2], merged.size(), read);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
