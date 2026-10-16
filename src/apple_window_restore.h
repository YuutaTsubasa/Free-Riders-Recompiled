#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreFoundation/CoreFoundation.h>

namespace sfr {

// After a crash, macOS asks at the next start whether to reopen the app's
// windows, in a modal dialog shown before the first window: the game seemed
// to hang at its start until someone answered it. Nothing here restores
// windows, so the question is turned off for this app.
inline void skip_window_restore_prompt() {
    CFPreferencesSetAppValue(CFSTR("ApplePersistenceIgnoreState"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
}

}  // namespace sfr
#endif
#endif
