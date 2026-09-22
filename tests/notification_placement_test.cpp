#include "notification_placement.h"
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        using Placement = sfr::NotificationPlacement;
        Placement placement;
        require(!placement.selected(), "no game placement request exists initially");
        placement.set(2);
        require(placement.selected()->horizontal == Placement::Horizontal::center &&
                placement.selected()->vertical == Placement::Vertical::bottom,
                "original constructor requests bottom center");
        placement.set(5);
        require(placement.selected()->horizontal == Placement::Horizontal::left &&
                placement.selected()->vertical == Placement::Vertical::top,
                "later original call requests top left");
        for (const auto flags : {3u, 7u, 12u, 16u, 0xffffffffu}) {
            bool rejected = false;
            try { placement.set(flags); } catch (const sfr::RuntimeStop&) { rejected = true; }
            require(rejected && placement.selected()->flags == 5, "conflicting or unknown flags preserve previous request");
        }
        placement.set(0);
        require(placement.selected()->horizontal == Placement::Horizontal::center &&
                placement.selected()->vertical == Placement::Vertical::center,
                "zero is an explicit center request, not absence of a setting");
        std::cout << "Notification placement checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
