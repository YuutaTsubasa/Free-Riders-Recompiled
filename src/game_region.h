#pragma once
#include <cstdint>
#include <string_view>

namespace sfr {
class GameRegion {
public:
    explicit GameRegion(std::string_view option = {});
    bool configured() const noexcept { return configured_; }
    uint32_t query() const;
private:
    bool configured_ = false;
};
}
