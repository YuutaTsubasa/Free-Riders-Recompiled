// Android entry point of the game. The launcher (LauncherActivity) writes
// settings.env and starts GameActivity, whose SDL activity loads libSDL2.so and
// libmain.so and calls SDL_main on its own thread. The game runs from the app's external
// files directory (/sdcard/Android/data/<package>/files):
//   game/image, game/assets   the installed game (as the launcher installs it)
//   shaders.pack              the compiled shaders (scripts/pack_shaders.py)
//   settings.env              optional NAME=VALUE lines for the runtime (read by
//                             GameActivity before the libraries load)
//   game.log                  the runtime's trace output (rewritten each start)
#include <SDL.h>
#include <android/log.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>

int sfr_game_main(int argc, char** argv);

extern "C" int SDL_main(int, char**) {
    const char* root = SDL_AndroidGetExternalStoragePath();
    if (!root || chdir(root) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "FreeRiders", "no external files directory: %s", SDL_GetError());
        return 1;
    }
    // Back is the game's own button (B); held, it leaves (native_presentation.cpp).
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!std::freopen("game.log", "w", stderr))
        __android_log_print(ANDROID_LOG_WARN, "FreeRiders", "cannot write %s/game.log", root);
    std::setvbuf(stderr, nullptr, _IOLBF, 1 << 16);  // whole lines, so a crash keeps the trace
    // GameActivity.loadLibraries has put settings.env and the defaults in the
    // environment already: the runtime reads them while libmain.so loads.
    const char* region = std::getenv("SFR_GAME_REGION");
    std::string region_argument = std::string("--game-region=") + (region && *region ? region : "ntsc-us");
    char program[] = "FreeRiders", image[] = "game/image", assets[] = "game/assets";
    char* arguments[] = {program, image, assets, region_argument.data(), nullptr};
    __android_log_print(ANDROID_LOG_INFO, "FreeRiders", "starting in %s", root);
    const int code = sfr_game_main(4, arguments);
    std::fflush(stderr);
    __android_log_print(ANDROID_LOG_INFO, "FreeRiders", "game exited with %d (see game.log)", code);
    // End the game's process (android:process=":game"): the next run needs
    // fresh static state, and the launcher shows again once this is gone.
    _exit(code);
}
