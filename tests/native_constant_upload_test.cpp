#include "native_constant_upload.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        sfr::NativeConstantUpload cache;
        sfr::NativeConstantUpload::Constants values{};
        std::array<uint8_t, 6 * sizeof(values)> storage;
        storage.fill(0xa5);
        auto first = cache.upload(values, 0, storage.data());
        require(!first.reused && first.offset == 0, "initial zeros are actually uploaded at offset zero");
        require(std::memcmp(storage.data(), values.data(), sizeof(values)) == 0, "first upload contains exact bytes");
        auto hit = cache.upload(values, sizeof(values), storage.data() + sizeof(values));
        require(hit.reused && hit.offset == 0, "identical bytes reuse the first immutable offset");
        require(std::all_of(storage.begin() + sizeof(values), storage.end(), [](uint8_t x) { return x == 0xa5; }),
                "a hit does not touch the new destination");
        values.back() = 0x80000000; // -0 differs from +0, even at the last register.
        auto changed = cache.upload(values, sizeof(values), storage.data() + sizeof(values));
        require(!changed.reused && changed.offset == sizeof(values), "changed final word forces a fresh upload");
        require(std::all_of(storage.begin(), storage.begin() + sizeof(values), [](uint8_t x) { return x == 0; }),
                "later uploads cannot mutate an earlier draw's constants");
        values.front() = 0x7fc00001;
        require(!cache.upload(values, 8192, storage.data() + 8192).reused, "new NaN payload uploads");
        require(cache.upload(values, 12288, storage.data() + 12288).reused, "identical NaN payload reuses bitwise");
        values.front() = 0x7fc00002;
        require(!cache.upload(values, 12288, storage.data() + 12288).reused, "different NaN payload uploads");
        cache.reset();
        storage.fill(0x5a); // simulates a reused ring, including the old offset.
        auto reset = cache.upload(values, 16384, storage.data() + 16384);
        require(!reset.reused && reset.offset == 16384, "ring reset invalidates identical cached data");
        require(std::memcmp(storage.data() + reset.offset, values.data(), sizeof(values)) == 0, "fresh ring receives exact bytes");
        std::cout << "Constant upload lifetime checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
