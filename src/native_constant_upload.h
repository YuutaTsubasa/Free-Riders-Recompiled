#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace sfr {
class NativeConstantUpload {
public:
    using Constants = std::array<uint32_t, 1024>;
    struct Result { uint64_t offset; bool reused; };
    // The caller resets at every upload-ring flush, before storage can be
    // overwritten or a different ring becomes current. Never inspect GPU memory.
    void reset() noexcept { valid_ = false; }
    Result upload(const Constants& values, uint64_t offset, uint8_t* destination) {
        if (valid_ && std::memcmp(previous_.data(), values.data(), sizeof(values)) == 0)
            return {offset_, true};
        std::memcpy(destination, values.data(), sizeof(values));
        previous_ = values;
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
