#pragma once
// Which SDL video driver the Linux launcher and game ask for (Issue #365).
//
// SDL 2 picks X11 first, so on a Wayland desktop both ran through XWayland.
// They now ask for Wayland first and X11 after it: a Wayland session gets
// native Wayland windows (the game's Vulkan surface is then a Wayland one,
// as SDL names VK_KHR_wayland_surface for the window), and SDL falls back to
// X11 where there is no Wayland compositor. Plume takes the surface
// extension SDL names (patches/plume-sdl-linux-surface.patch) and the swap
// chain's size from SDL's drawable size, so nothing else depends on which.
//
// Left as before (SDL's own choice, X11):
//  - when the player set SDL_VIDEODRIVER: theirs is used;
//  - with SDL older than 2.24 (Ubuntu 22.04 has 2.0.20): it takes no list
//    of drivers to try in turn, so "wayland,x11" would start no video at
//    all, and before 2.0.22 it reads the choice from the environment only.
// The launcher's Test option "Use X11" (SFR_TEST_X11=1) asks for X11 only.
#include <string_view>
// SFR_VIDEO_DRIVER_RULE_ONLY: the rule alone, for a test without SDL.
#if defined(__linux__) && !defined(__ANDROID__) && !defined(SFR_VIDEO_DRIVER_RULE_ONLY)
#define SFR_VIDEO_DRIVER_SDL 1
#include <SDL.h>
#include <cstdlib>
#endif

namespace sfr {
// The SDL_VIDEODRIVER hint to set before SDL starts video, or nullptr to
// leave SDL's own choice. player is the environment's SDL_VIDEODRIVER,
// force_x11 the Test option's value (SFR_TEST_X11), version SDL's at run
// time as SDL_VERSIONNUM gives it (major * 1000 + minor * 100 + patch).
inline const char* linux_video_driver_hint(const char* player, const char* force_x11, int version) {
    if (player && *player) return nullptr;
    if (force_x11 && std::string_view(force_x11) == "1") return "x11";
    if (version < 2 * 1000 + 24 * 100) return nullptr;
    return "wayland,x11";
}

#ifdef SFR_VIDEO_DRIVER_SDL
// Sets the hint (before SDL_Init or the first SDL_InitSubSystem of video)
// and returns it, or nullptr when SDL is left to choose. force_x11 is the
// Test option's value: the game's environment, or the launcher's settings.
inline const char* choose_linux_video_driver(const char* force_x11) {
    SDL_version linked{};
    SDL_GetVersion(&linked);
    const char* hint = linux_video_driver_hint(std::getenv("SDL_VIDEODRIVER"), force_x11,
                                               SDL_VERSIONNUM(linked.major, linked.minor, linked.patch));
    // By name: SDL_HINT_VIDEODRIVER is 2.0.22's, newer than Ubuntu 22.04's headers.
    if (hint) SDL_SetHint("SDL_VIDEODRIVER", hint);
    return hint;
}

// The game's choice, from its environment (the launcher exports the Test option).
inline const char* choose_game_video_driver() { return choose_linux_video_driver(std::getenv("SFR_TEST_X11")); }
#endif
}
