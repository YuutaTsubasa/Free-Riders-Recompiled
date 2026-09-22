#pragma once
#include <cstdint>

namespace sfr {
// No native game profile has been selected in this runtime. This backend must
// be replaced by the real slot/profile owner when selection is implemented;
// host Windows login is not a game sign-in operation.
uint32_t unselected_user_signin_state(uint32_t user_index);
}
