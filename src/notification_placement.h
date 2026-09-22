#pragma once
#include "guest_memory.h"
#include <optional>

namespace sfr {
// Popup alignment requested by the game. This is presentation configuration;
// setting it does not open UI or enqueue a UI-state notification.
class NotificationPlacement {
public:
    enum class Horizontal { center, left, right };
    enum class Vertical { center, top, bottom };
    struct Selection { uint32_t flags; Horizontal horizontal; Vertical vertical; };
    void set(uint32_t flags) {
        if ((flags & ~uint32_t{15}) || (flags & 3) == 3 || (flags & 12) == 12)
            throw RuntimeStop("notification-placement", flags, "conflicting or unknown popup alignment bits");
        selected_ = Selection{flags,
            flags & 4 ? Horizontal::left : flags & 8 ? Horizontal::right : Horizontal::center,
            flags & 1 ? Vertical::top : flags & 2 ? Vertical::bottom : Vertical::center};
    }
    const std::optional<Selection>& selected() const { return selected_; }
private:
    std::optional<Selection> selected_;
};
}
