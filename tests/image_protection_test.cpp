#include "image_protection.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void put(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.at(offset + i) = uint8_t(value >> (24 - i * 8));
}
std::vector<uint8_t> header() {
    std::vector<uint8_t> data(0x1000);
    put(data, 0, 0x58455832); put(data, 8, 0x1000); put(data, 0x10, 0x80);
    put(data, 0x80, 0x184 + 3 * 24); put(data, 0x84, 0x40000);
    put(data, 0x80 + 0x10c, 8); put(data, 0x80 + 0x110, 0x82000000);
    put(data, 0x80 + 0x180, 3);
    put(data, 0x204, 0x11); // One code page.
    put(data, 0x21c, 0x22); // Two data pages.
    put(data, 0x234, 0x13); // One read-only resource page.
    return data;
}
template<class F> void require_stop(F operation, const char* category) {
    try { operation(); }
    catch (const sfr::RuntimeStop& error) { require(error.category == category, "stop category"); return; }
    throw std::runtime_error("expected checked stop");
}
void queries() {
    auto data = header();
    const auto original = data;
    sfr::ImageProtection image(data, 0x82000000, 0x40000);
    require(data == original, "metadata construction does not edit header");
    for (uint32_t offset : {0u, 1u, 0xffffu})
        require(image.query_address_protect(0x82000000 + offset) == 2, "code page is read-only");
    for (uint32_t offset : {0x10000u, 0x10001u, 0x20000u, 0x2ffffu})
        require(image.query_address_protect(0x82000000 + offset) == 4, "full multi-page data extent");
    require(image.query_address_protect(0x82039f00) == 2, "read-only embedded resource");
    require(image.query_address_protect(0x8203ffff) == 2, "last image byte");
    put(data, 0x234, 0x12);
    require(image.query_address_protect(0x82039f00) == 2, "caller header changes cannot change query metadata");
    require(image.contains(0x82000000) && image.contains(0x8203ffff), "image endpoints included");
    for (uint32_t address : {0u, 0x81ffffffu, 0x82040000u, 0xffffffffu}) {
        require(!image.contains(address), "foreign address excluded");
        require_stop([&] { image.query_address_protect(address); }, "memory-protection");
    }
}
void malformed() {
    for (size_t size : {size_t(0), size_t(23), size_t(0x203), size_t(0x237)}) {
        auto data = header(); data.resize(size);
        require_stop([&] { sfr::ImageProtection image(data, 0x82000000, 0x40000); }, "image-protection");
    }
    const std::pair<size_t, uint32_t> changes[] = {
        {0, 0}, {8, 0x1001}, {0x10, 0}, {0x10, 0x81}, {0x10, 0xfffffff0},
        {0x14, 0x10000}, {0x80, 0x183}, {0x80, 0xfffffff0},
        {0x84, 0x30000}, {0x190, 0x82010000}, {0x200, 0}, {0x200, 0xffffffff},
        {0x204, 1}, {0x204, 0x10}, {0x204, 0x14}, {0x204, 0xfffffff1},
        {0x21c, 0x12}, {0x234, 0x23}
    };
    for (const auto& [offset, value] : changes) {
        auto data = header(); put(data, offset, value);
        require_stop([&] { sfr::ImageProtection image(data, 0x82000000, 0x40000); }, "image-protection");
    }
    const auto data = header();
    for (auto [base, size] : {std::pair{0x82000001u, 0x40000u}, {0x82000000u, 0u},
                             {0x82000000u, 0x40001u}, {0xffff0000u, 0x40000u}})
        require_stop([&, base = base, size = size] { sfr::ImageProtection image(data, base, size); }, "image-protection");
}
}
int main() {
    try { queries(); malformed(); std::cout << "Image protection checks passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
