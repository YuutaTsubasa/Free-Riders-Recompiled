package com.freeriders.recompiled;

import android.app.Activity;
import android.content.Intent;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

// Firebase Test Lab's game loop (com.google.intent.action.TEST_LOOP) starts
// this: it runs the game's graphics start-up alone in GameActivity
// (src/graphics_selftest.cpp), which needs no installed game, and finishes
// when that process has ended, which ends the loop. The steps are in logcat
// (tag FreeRiders, GRAPHICS_SELFTEST) and in game.log.
//
// An APK with a bundled shaders.pack also has the self test prepare every
// pipeline the game prepares at start. The loop's scenario picks how the
// vertex shaders' clip distance is treated (SFR_TEST_NO_CLIP_DISTANCE): 1
// keeps it, 2 removes it, any other as the device's features say.
public class GraphicsTestActivity extends Activity {
    static final String EXTRA = "com.freeriders.recompiled.GRAPHICS_SELFTEST";
    static final String CLIP_DISTANCE = "com.freeriders.recompiled.GRAPHICS_SELFTEST_CLIP_DISTANCE";
    private boolean started;

    @Override
    protected void onResume() {
        super.onResume();
        if (started) {
            finish();
            return;
        }
        started = true;
        File directory = getExternalFilesDir(null);
        if (directory != null) {
            for (String name : new String[] { "shaders.pack", "pipelines-vulkan.manifest" }) copyAsset(name, new File(directory, name));
        }
        int scenario = getIntent() == null ? 0 : getIntent().getIntExtra("scenario", 0);
        Intent intent = new Intent(this, GameActivity.class).putExtra(EXTRA, true);
        if (scenario == 1) intent.putExtra(CLIP_DISTANCE, "0");
        if (scenario == 2) intent.putExtra(CLIP_DISTANCE, "1");
        startActivity(intent);
    }

    private void copyAsset(String name, File target) {
        try (InputStream in = getAssets().open(name); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[1 << 16];
            for (int read; (read = in.read(buffer)) > 0; ) out.write(buffer, 0, read);
        } catch (Exception error) {
            target.delete();  // an APK without it
        }
    }
}
