#include "unselected_users.h"
#include "guest_memory.h"

namespace sfr {
uint32_t unselected_user_signin_state(uint32_t user_index) {
    if (user_index >= 4)
        throw RuntimeStop("user-signin", user_index, "only ordinary user slots 0 through 3 are supported");
    return 0;
}
}
