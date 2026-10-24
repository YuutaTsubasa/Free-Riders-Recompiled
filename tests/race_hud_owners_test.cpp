#include "race_hud_owners.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace sfr::race_hud;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Return-address chains as SFR_ASPECT_TRACE printed them in a World Grand Prix
// Ring Challenge at 21:9 (Issue #274), innermost first.
const std::vector<uint32_t> ring_icon{0x824C0C88, 0x824AC1C8, 0x824AD1A0, 0x824AD1A0, 0x823FEAA8, 0x824087A8};
const std::vector<uint32_t> ring_digit{0x824C0C88, 0x824AC1C8, 0x824AD1A0, 0x824AD1A0, 0x823FEAC8, 0x824087A8};
const std::vector<uint32_t> left_frame{0x824C0C88, 0x824AC1C8, 0x82402088, 0x824087A8};
const std::vector<uint32_t> time_panel{0x824C0C88, 0x824AC1C8, 0x8240A5D4, 0x8240878C};
const std::vector<uint32_t> kinect_player{0x824C6A04, 0x8223CED8, 0x82432970, 0x824327A4};

void ring_challenge() {
    // The panel and its numbers go to the same edge.
    require(chain_side(left_frame, false) == -1, "the left column's frame goes to the left edge");
    require(chain_side(ring_icon, false) == by_half, "the mission's ring pieces go by their half");
    require(chain_side(ring_digit, false) == by_half, "the mission's ring digits go by their half");
    // Boxes from the trace: ring icon, the big digits, "/100", the record's rings.
    require(half_side(62.8f, 126.8f, 1280.0f) == -1, "the ring icon is in the left half");
    require(half_side(217.0f, 267.4f, 1280.0f) == -1, "the third digit is in the left half");
    require(half_side(350.0f, 383.6f, 1280.0f) == -1, "the target's last digit is in the left half");
    require(half_side(166.0f, 199.6f, 1280.0f) == -1, "the record's rings are in the left half");
    for (uint32_t site : {0x823FE9E8u, 0x823FEA48u, 0x823FEAA8u, 0x823FEB48u, 0x823FEBB8u, 0x823FEAC8u,
                          0x823FEAE0u, 0x823FEBD8u, 0x823FEBF0u, 0x823FEC28u})
        require(owner_side(site, false) == by_half && owner_side(site, true) == by_half,
                "every draw of the mission counters is placed");
    // What stays in the middle still does.
    require(chain_side(time_panel, false) == 0, "TIME stays in the middle");
    require(chain_side(kinect_player, false) == 0, "the player figure stays in the middle");
}

void race_hud_unchanged() {
    require(owner_side(0x82401C9C, false) == -1 && owner_side(0x82401CE0, true) == -1, "rings and LV stay left");
    require(owner_side(0x8240069C, false) == -1, "LAP and RECORD stay left");
    require(owner_side(0x823EFF80, false) == +1 && owner_side(0x82403158, true) == +1, "the map stays right");
    require(owner_side(0x823F97E4, false) == +1 && owner_side(0x823F97E4, true) == by_half, "the gauge as before");
    require(owner_side(0x823FC63C, false) == 0 && owner_side(0x823FC63C, true) == by_half, "the gauge scale as before");
    require(owner_side(0x824A7510, false) == 0, "text stays in the middle");
    require(owner_side(0x8241079C, false) == 0, "the results stay in the middle");
    require(chain_side(std::vector<uint32_t>{0x824C0C88, 0x824AC1C8}, false) == 0, "only the generic layer: middle");
    require(chain_side(std::vector<uint32_t>{}, false) == 0, "no chain: middle");
    require(half_side(1028.0f, 1100.0f, 1280.0f) == +1, "a piece in the right half goes right");
}
}  // namespace

int main() {
    try {
        ring_challenge();
        race_hud_unchanged();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "race_hud_owners: ok\n";
    return 0;
}
