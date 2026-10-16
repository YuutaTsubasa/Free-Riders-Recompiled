// iOS entry point of the game. The app is one process: its program is the
// launcher, and Play loads this library (FreeRidersGame.framework) once the
// launcher has put the settings in the environment, which the runtime reads
// while the library loads (as Android's game activity does before loading
// libmain.so). The launcher calls this by name; the game's own main is
// compiled as sfr_game_main.
int sfr_game_main(int argc, char** argv);

extern "C" __attribute__((visibility("default"))) int sfr_ios_game_main(int argc, char** argv) {
    return sfr_game_main(argc, argv);
}
