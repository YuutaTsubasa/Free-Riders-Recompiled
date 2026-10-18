#include "bounded_descriptor_cache.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using Cache = sfr::BoundedSetCache<uint64_t>;

Cache::Key key(uint32_t first, uint32_t second = 0) {
    Cache::Key k{};
    k[0] = first;
    k[1] = second;
    return k;
}

// A set made once is found again, across as many lookups as there are
// draws and frames, while its textures keep their views.
void keeps_a_set_while_its_textures_stay() {
    sfr::BoundedTextureStamps stamps(64);
    Cache cache;
    stamps.touch(5);
    stamps.touch(6);
    auto found = cache.textures(key(5, 6), stamps);
    require(found.make, "a new key makes a set");
    *found.set = 42;
    for (int draw = 0; draw < 100; ++draw) {
        found = cache.textures(key(5, 6), stamps);
        require(!found.make && *found.set == 42, "an unchanged key finds its set");
    }
    // Another index changing leaves it alone.
    stamps.touch(7);
    found = cache.textures(key(5, 6), stamps);
    require(!found.make && *found.set == 42, "another index changing keeps the set");
    require(cache.kept() == 1 && cache.abandoned() == 0, "one set kept, none abandoned");
}

// A set whose index got a new view is made again, never rewritten: the old
// one may be bound by a draw recorded earlier.
void makes_a_set_again_when_a_texture_changes() {
    sfr::BoundedTextureStamps stamps(64);
    Cache cache;
    stamps.touch(5);
    auto found = cache.textures(key(5, 6), stamps);
    *found.set = 1;
    stamps.touch(6);
    found = cache.textures(key(5, 6), stamps);
    require(found.make, "a changed index makes the set again");
    *found.set = 2;
    found = cache.textures(key(5, 6), stamps);
    require(!found.make && *found.set == 2, "the new set is found after");
    require(cache.abandoned() == 1 && cache.kept() == 1, "the old set is abandoned");
    // Indices beyond the stamps (none) never change.
    found = cache.textures(key(1000), stamps);
    require(found.make, "an unknown key makes a set");
    found = cache.textures(key(1000), stamps);
    require(!found.make, "an index beyond the stamps never changes");
}

void keeps_sampler_sets() {
    Cache cache;
    auto found = cache.samplers(key(3, 4));
    require(found.make, "a new sampler key makes a set");
    *found.set = 9;
    found = cache.samplers(key(3, 4));
    require(!found.make && *found.set == 9, "a sampler set is found again");
    found = cache.samplers(key(4, 3));
    require(found.make, "another order is another set");
}

// The ring starts over once too many sets are abandoned or kept.
void grows_crowded() {
    sfr::BoundedTextureStamps stamps(4096);
    Cache cache;
    for (uint32_t i = 0; i < Cache::max_abandoned; ++i) {
        require(!cache.crowded(), "not crowded before the limit");
        stamps.touch(10);
        *cache.textures(key(10), stamps).set = i;
        if (i) require(cache.abandoned() == i, "each change abandons a set");
    }
    stamps.touch(10);
    cache.textures(key(10), stamps);
    require(cache.crowded(), "crowded at the abandoned limit");
    cache.clear();
    require(!cache.crowded() && cache.kept() == 0 && cache.abandoned() == 0, "clear starts over");
    require(cache.textures(key(10), stamps).make, "nothing is found after clear");

    Cache many;
    for (uint32_t i = 0; i < Cache::max_kept; ++i) many.samplers(key(i));
    require(many.crowded(), "crowded at the kept limit");
}
}  // namespace

int main() {
    try {
        keeps_a_set_while_its_textures_stay();
        makes_a_set_again_when_a_texture_changes();
        keeps_sampler_sets();
        grows_crowded();
        std::cout << "bounded descriptor cache tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
