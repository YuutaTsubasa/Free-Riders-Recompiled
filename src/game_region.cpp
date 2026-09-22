#include "game_region.h"
#include "guest_memory.h"

namespace sfr {
GameRegion::GameRegion(std::string_view option) {
    if (option.empty()) return;
    if (option != "--game-region=ntsc-us")
        throw RuntimeStop("game-region", 0, "unsupported game-region option; expected --game-region=ntsc-us");
    configured_ = true;
}
uint32_t GameRegion::query() const {
    if (!configured_)
        throw RuntimeStop("game-region", 0x82ACB24C, "supply an explicit --game-region=ntsc-us profile");
    return 0x00FF;
}
}
