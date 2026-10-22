// The title's depth view as the recompiler generated it (82439998 and
// 82439D80, copied out of out/recomp/diagnostic at build time by
// extract_guest_functions.cmake), run on test images in a guest memory of
// its own, against the offline extractor (kinect_depth_features.h): every
// field the game writes must come out the same, floats bit for bit. Built
// with the diagnostic only (it needs the generated sources), and with the
// diagnostic's floating-point options for the generated code.
//
// The two functions read four constants from the game's image, given here
// as the decoded image has them: the vperm tables at 8217D100 and 8217D110
// (horizontal sums), 0.25 at 8218FCE4 and 65528.0 x4 at 821AC140 (an empty
// part's mean, copied once into 83E54520 behind the flag word 83E54530).
#include "ppc_context.h"
#include "vector_integer.h"

#include "kinect_depth_features.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace sfr {
// What the generated code calls besides guest memory, as the diagnostic's
// hooks would but with no thread to schedule (diagnostic_hooks.h).
uint8_t* test_base = nullptr;
template<class C> inline void enter_function(C&, const char*, uint32_t) {}
template<class C> inline void guest_checkpoint(C&) {}
inline void load_vector_memory(uint32_t address, uint8_t (&destination)[16]) {
    const uint8_t* source = test_base + (address & ~0xFu);
    for (int i = 0; i < 16; ++i) destination[i] = source[15 - i];
}
inline void store_vector_memory(uint32_t address, const uint8_t (&source)[16]) {
    uint8_t* destination = test_base + (address & ~0xFu);
    for (int i = 0; i < 16; ++i) destination[i] = source[15 - i];
}
}
#define SFR_FAST_PATH() ((void)0)
// The register save and restore routines the functions branch to: nothing
// here depends on the registers they keep.
PPC_FUNC(__savegprlr_19) { (void)ctx; (void)base; }
PPC_FUNC(__restgprlr_19) { (void)ctx; (void)base; }

#include "kinect_depth_view_guest.inc"

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr uint32_t width = 320, height = 240;
constexpr uint32_t stack_top = 0x7000F000, image_at = 0x40000000, manager_at = 0x50000000, record_at = 0x50001000;

uint8_t* reserve() {
#ifdef _WIN32
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(1) << 32, MEM_RESERVE, PAGE_NOACCESS));
    const auto commit = [&](uint32_t at, uint32_t size) {
        require(VirtualAlloc(base + at, size, MEM_COMMIT, PAGE_READWRITE) != nullptr, "commit");
    };
    require(base != nullptr, "reserve 4 GB");
    commit(0x70000000, 0x10000);
    commit(image_at, 0x40000);
    commit(manager_at, 0x10000);
    commit(0x8217D000, 0x1000);
    commit(0x8218F000, 0x1000);
    commit(0x821AC000, 0x1000);
    commit(0x83E54000, 0x1000);
    return base;
#else
    void* base = mmap(nullptr, size_t(1) << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    require(base != MAP_FAILED, "reserve 4 GB");
    return static_cast<uint8_t*>(base);
#endif
}

void put32(uint8_t* base, uint32_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) base[at + i] = uint8_t(value >> (24 - 8 * i));
}
uint32_t get32(const uint8_t* base, uint32_t at) {
    return uint32_t(base[at]) << 24 | uint32_t(base[at + 1]) << 16 | uint32_t(base[at + 2]) << 8 | base[at + 3];
}
float getf(const uint8_t* base, uint32_t at) {
    const uint32_t bits = get32(base, at);
    float value = 0;
    std::memcpy(&value, &bits, 4);
    return value;
}
uint32_t bits(float value) {
    uint32_t out = 0;
    std::memcpy(&out, &value, 4);
    return out;
}

void constants(uint8_t* base) {
    const uint8_t first[16] = {8, 9, 10, 11, 12, 13, 14, 15, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8_t second[16] = {4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3};
    std::memcpy(base + 0x8217D100, first, 16);
    std::memcpy(base + 0x8217D110, second, 16);
    put32(base, 0x8218FCE4, 0x3E800000);  // 0.25
    for (uint32_t i = 0; i < 4; ++i) put32(base, 0x821AC140 + 4 * i, 0x477FF800);  // 65528.0
}

struct Guest {
    sfr::KinectDepthCentroid centroid;
    std::array<uint32_t, 10> record{};  // +640..+679 as words
};

Guest run(uint8_t* base, const std::vector<uint16_t>& image, uint32_t slot) {
    for (size_t i = 0; i < image.size(); ++i) {
        base[image_at + 2 * i] = uint8_t(image[i] >> 8);  // big-endian, as the game's buffer
        base[image_at + 2 * i + 1] = uint8_t(image[i]);
    }
    std::memset(base + manager_at, 0xCD, 0x200);
    for (uint32_t i = 0; i < 10; ++i) put32(base, record_at + 4 * i, 0xDEADBEEF);  // what an older image left
    // The host's floating-point state as a guest thread starts with it (its
    // exceptions masked); the generated code turns flushing on and off in it,
    // and it is put back after, for the extractor.
    const uint32_t host_csr = simde_mm_getcsr();
    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = stack_top;
    ctx.r3.u64 = slot;
    ctx.r4.u64 = manager_at;
    ctx.r5.u64 = image_at;
    ctx.r6.u64 = width;
    ctx.r7.u64 = height;
    __imp__sub_82439998(ctx, base);
    Guest out;
    const uint32_t at = manager_at + 8 * slot;
    out.centroid.column = get32(base, at);
    out.centroid.row = get32(base, at + 4);
    out.centroid.min_column = get32(base, at + 48);
    out.centroid.min_row = get32(base, at + 52);
    out.centroid.max_column = get32(base, at + 96);
    out.centroid.max_row = get32(base, at + 100);
    PPCContext second{};
    second.fpscr.loadFromHost();
    second.r1.u64 = stack_top;
    second.r3.u64 = slot;
    second.r4.u64 = record_at;
    second.r5.u64 = at;
    second.r6.u64 = image_at;
    second.r7.u64 = width;
    second.r8.u64 = height;
    __imp__sub_82439D80(second, base);
    simde_mm_setcsr(host_csr);
    for (uint32_t i = 0; i < 10; ++i) out.record[i] = get32(base, record_at + 4 * i);
    return out;
}

// What a game might be shown: up to three players as blobs of a few
// rectangles, with depth ramps, noise, holes, players without a depth,
// players at the image's edges, and now and then an image full of one
// player at the deepest value (the sums' saturation).
std::vector<uint16_t> image_for(std::mt19937& random, uint32_t kind) {
    std::vector<uint16_t> image(size_t(width) * height, 0);
    std::uniform_int_distribution<uint32_t> any(0, 0xFFFF);
    if (kind % 37 == 5) {
        std::fill(image.begin(), image.end(), uint16_t(0xFFF8 | 1));
        return image;
    }
    if (kind % 41 == 7) {
        for (auto& p : image) p = uint16_t(any(random));
        return image;
    }
    // A background.
    std::uniform_int_distribution<uint32_t> background(0, 3);
    const uint32_t back = background(random);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            if (back) image[size_t(y) * width + x] = uint16_t((2500 + x * back + y) << 3);
    std::uniform_int_distribution<uint32_t> players(0, 3), index(1, 6), size(1, 120), depth(400, 4100), share(0, 99);
    const uint32_t count = kind % 11 == 0 ? 0 : players(random);
    for (uint32_t p = 0; p < count; ++p) {
        const uint32_t player = index(random);
        const uint32_t base_depth = depth(random);
        const uint32_t pieces = 1 + size(random) % 4;
        for (uint32_t piece = 0; piece < pieces; ++piece) {
            const uint32_t w = size(random) % (kind % 7 == 3 ? 6 : 90) + 1, h = size(random) % (kind % 7 == 3 ? 6 : 140) + 1;
            const uint32_t x0 = std::uniform_int_distribution<uint32_t>(0, width - 1)(random);
            const uint32_t y0 = std::uniform_int_distribution<uint32_t>(0, height - 1)(random);
            for (uint32_t y = y0; y < std::min(height, y0 + h); ++y)
                for (uint32_t x = x0; x < std::min(width, x0 + w); ++x) {
                    const uint32_t roll = share(random);
                    uint32_t mm = base_depth + (x - x0) * 3 + (y - y0) + (roll % 13);
                    if (roll < 3) mm = 0;  // a player pixel without a depth
                    if (roll > 96) continue;  // a hole
                    image[size_t(y) * width + x] = uint16_t(std::min<uint32_t>(mm, 8191) << 3 | player);
                }
        }
    }
    // A few lone pixels of any player.
    for (uint32_t i = 0; i < share(random) % 20; ++i)
        image[std::uniform_int_distribution<size_t>(0, image.size() - 1)(random)] = uint16_t(any(random));
    return image;
}
}

int main() {
    try {
        uint8_t* base = reserve();
        sfr::test_base = base;
        constants(base);
        std::mt19937 random(20261019);
        uint64_t compared = 0, written = 0, sparse = 0;
        for (uint32_t kind = 0; kind < 600; ++kind) {
            const auto image = image_for(random, kind);
            for (uint32_t slot = 0; slot < 6; ++slot) {
                const Guest guest = run(base, image, slot);
                const sfr::KinectDepthFeatures tool = sfr::kinect_depth_view_features(image.data(), width, height, slot);
                const std::string where = "image " + std::to_string(kind) + " slot " + std::to_string(slot) + ": ";
                const auto& g = guest.centroid;
                const auto& t = tool.centroid;
                require(g.column == t.column && g.row == t.row, where + "centroid " + std::to_string(g.column) + "," +
                                                                    std::to_string(g.row) + " vs " + std::to_string(t.column) +
                                                                    "," + std::to_string(t.row));
                require(g.min_column == t.min_column && g.min_row == t.min_row && g.max_column == t.max_column &&
                            g.max_row == t.max_row,
                        where + "extents " + std::to_string(g.min_column) + "," + std::to_string(g.min_row) + "-" +
                            std::to_string(g.max_column) + "," + std::to_string(g.max_row) + " vs " +
                            std::to_string(t.min_column) + "," + std::to_string(t.min_row) + "-" +
                            std::to_string(t.max_column) + "," + std::to_string(t.max_row));
                for (uint32_t i = 0; i < 4; ++i)
                    require(guest.record[i] == bits(tool.split.counts[i]),
                            where + "count " + std::to_string(i) + " " + std::to_string(getf(base, record_at + 4 * i)) +
                                " vs " + std::to_string(tool.split.counts[i]));
                if (tool.split.written) {
                    ++written;
                    for (uint32_t i = 0; i < 4; ++i)
                        require(guest.record[4 + i] == bits(tool.split.means[i]),
                                where + "mean " + std::to_string(i) + " " + std::to_string(getf(base, record_at + 16 + 4 * i)) +
                                    " vs " + std::to_string(tool.split.means[i]));
                    require(guest.record[8] == tool.split.near_count,
                            where + "near " + std::to_string(guest.record[8]) + " vs " + std::to_string(tool.split.near_count));
                    require(guest.record[9] == bits(tool.split.average), where + "average");
                } else {
                    ++sparse;
                    for (uint32_t i = 4; i < 10; ++i)
                        require(guest.record[i] == 0xDEADBEEF, where + "a sparse image writes only the counts");
                }
                ++compared;
            }
        }
        require(written > 200 && sparse > 200, "the images exercise both outcomes");
        std::cout << "kinect_depth_view_guest: " << compared << " (image, slot) pairs identical to the generated code ("
                  << written << " written, " << sparse << " too sparse)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "kinect_depth_view_guest_test: " << error.what() << '\n';
        return 1;
    }
}
