#include "local_profile.h"
#include "guest_memory.h"
#include <cstdlib>

namespace sfr {
LocalProfile make_local_profile(const char* enabled, const char* name) {
    LocalProfile profile{enabled && *enabled == '1', 0, name && *name ? name : "Player"};
    if (profile.name.size() > 15) profile.name.resize(15);
    // A stable offline XUID from the name (FNV-1a), so saves follow the profile.
    uint64_t hash = 0xCBF29CE484222325ull;
    for (unsigned char c : profile.name) hash = (hash ^ c) * 0x100000001B3ull;
    profile.xuid = 0xE000000000000000ull | (hash & 0x0000FFFFFFFFFFFFull);
    return profile;
}

const LocalProfile& local_profile() {
    static const LocalProfile profile = make_local_profile(std::getenv("SFR_PROFILE"), std::getenv("SFR_PROFILE_NAME"));
    return profile;
}

const LocalProfile* profile_for(uint32_t user_index) {
    if (user_index >= 4)
        throw RuntimeStop("user-signin", user_index, "only ordinary user slots 0 through 3 are supported");
    return user_index == 0 && local_profile().signed_in ? &local_profile() : nullptr;
}

uint32_t profile_signin_state(uint32_t user_index) { return profile_for(user_index) ? 1 : 0; }
}
