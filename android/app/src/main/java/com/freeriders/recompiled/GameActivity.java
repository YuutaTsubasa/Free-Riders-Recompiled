package com.freeriders.recompiled;

import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import org.libsdl.app.SDLActivity;

// SDL's activity runs the game's SDL_main (src/android_main.cpp) in libmain.so.
public class GameActivity extends SDLActivity {
    // Playing, as the desktop launcher starts the game (launcher_settings.cpp).
    private static final String[][] DEFAULTS = {
        {"SFR_CALL_BUDGET", "18446744073709551615"},
        {"SFR_WATCHDOG_SECONDS", "31536000"},
        {"SFR_ALLOW_RENDER_TARGETS", "1"},
        {"SFR_TRACE_GRAPHICS", "0"},
        {"SFR_DIAGNOSTIC_ENTRIES", "0"},
        {"SFR_TRACE_IMPORTS", "0"},
        {"SFR_FRAME_LIMIT", "60"},
        {"SFR_RENDER_EVERY", "1"},
        {"SFR_PARALLEL_WORKER", "cores"},
        {"SFR_VERTEX_CACHE", "1"},
        {"SFR_AUDIO", "1"},
        {"SFR_PROFILE", "1"},
        {"SFR_SHADER_PACK", "shaders.pack"},
    };

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL2", "main" };
    }

    // The runtime reads its settings while libmain.so loads (static
    // initializers), so they are in the environment first: settings.env in
    // the app's external files directory (NAME=VALUE lines), then defaults.
    @Override
    public void loadLibraries() {
        File directory = getExternalFilesDir(null);
        if (directory != null) {
            File settings = new File(directory, "settings.env");
            if (settings.isFile()) {
                try (BufferedReader reader = new BufferedReader(new FileReader(settings))) {
                    for (String line; (line = reader.readLine()) != null;) {
                        line = line.trim();
                        int equals = line.indexOf('=');
                        if (line.isEmpty() || line.startsWith("#") || equals <= 0) continue;
                        setenv(line.substring(0, equals), line.substring(equals + 1), true);
                    }
                } catch (IOException error) {
                    Log.w("FreeRiders", "cannot read settings.env", error);
                }
            }
        }
        for (String[] setting : DEFAULTS) setenv(setting[0], setting[1], false);
        super.loadLibraries();
    }

    private static void setenv(String name, String value, boolean overwrite) {
        try {
            Os.setenv(name, value, overwrite);
        } catch (ErrnoException error) {
            Log.w("FreeRiders", "cannot set " + name, error);
        }
    }
}
