#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace sfr {
class NativeConstantUpload {
public:
    using Constants = std::array<uint32_t, 1024>;
    struct Result { uint64_t offset; bool reused; };
    // The caller resets at every upload-ring flush, before storage can be
    // overwritten or a different ring becomes current. Never inspect GPU memory.
    void reset() noexcept { valid_ = false; }
    Result upload(std::span<const uint32_t, 1024> values, uint64_t offset, uint8_t* destination) {
        if (valid_ && std::memcmp(previous_.data(), values.data(), sizeof(previous_)) == 0)
            return {offset_, true};
        std::memcpy(destination, values.data(), sizeof(previous_));
        std::memcpy(previous_.data(), values.data(), sizeof(previous_));
        offset_ = offset;
        valid_ = true;
        return {offset, false};
    }
private:
    Constants previous_{};
    uint64_t offset_ = 0;
    bool valid_ = false;
};
}
