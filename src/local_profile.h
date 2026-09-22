#pragma once
#include <cstdint>
#include <string>

namespace sfr {
// The console profile of this runtime: one offline profile signed in on user
// slot 0, so the title can own save content and keep records (a title keeps
// none for a player who is not signed in). SFR_PROFILE=1 signs it in (off by
// default until saving is complete); SFR_PROFILE_NAME names it (at most 15
// characters, "Player" by default).
struct LocalProfile {
    bool signed_in;
    uint64_t xuid;      // offline XUID: 0xE000... (never a Live account)
    std::string name;   // gamertag
};
const LocalProfile& local_profile();
// XamUserGetSigninState: 1 (signed in locally) for the profile's slot, else 0.
uint32_t profile_signin_state(uint32_t user_index);
// The profile on this slot, or null.
const LocalProfile* profile_for(uint32_t user_index);
LocalProfile make_local_profile(const char* enabled, const char* name);
}
