#pragma once
#include "guest_memory.h"
#include <array>
#include <cstdint>

namespace sfr {
struct DisplayMode {
    uint32_t width;
    uint32_t height;
    float refresh_hz;
    bool interlaced;
};

DisplayMode query_native_display_mode();

class VideoMode {
public:
    static constexpr uint32_t structure_size = 48;
    VideoMode(GuestMemory& memory, DisplayMode mode);
    void query(uint32_t output) const;
    std::array<uint32_t, 12> snapshot() const;
private:
    GuestMemory& memory_;
    DisplayMode mode_;
};
}
