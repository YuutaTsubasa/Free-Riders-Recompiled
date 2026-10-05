package com.freeriders.recompiled;

import android.app.Activity;
import android.content.Intent;

// Firebase Test Lab's game loop (com.google.intent.action.TEST_LOOP) starts
// this: it runs the game's graphics start-up alone in GameActivity
// (src/graphics_selftest.cpp), which needs no installed game, and finishes
// when that process has ended, which ends the loop. The steps are in logcat
// (tag FreeRiders, GRAPHICS_SELFTEST) and in game.log.
public class GraphicsTestActivity extends Activity {
    static final String EXTRA = "com.freeriders.recompiled.GRAPHICS_SELFTEST";
    private boolean started;

    @Override
    protected void onResume() {
        super.onResume();
        if (started) {
            finish();
            return;
        }
        started = true;
        startActivity(new Intent(this, GameActivity.class).putExtra(EXTRA, true));
    }
}
