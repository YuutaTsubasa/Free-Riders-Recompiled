package com.freeriders.recompiled;

import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.libsdl.app.SDLActivity;

// The app's entry: the launcher (liblauncher.so, src/launcher_main.cpp) with
// its settings and install pages. The game runs in GameActivity, in a process
// of its own (android:process=":game"), because the runtime reads its
// settings while its library loads.
public class LauncherActivity extends SDLActivity {
    private static final int PICK_DOCUMENT = 1;

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL2", "launcher" };
    }

    // Called from the launcher's thread: shows the system's document picker;
    // the result arrives through nativeDocumentPicked.
    public void pickDocument() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            try {
                startActivityForResult(intent, PICK_DOCUMENT);
            } catch (Exception error) {
                Log.w("FreeRiders", "no document picker", error);
                nativeDocumentPicked("", "");
            }
        });
    }

    // Called from the launcher's thread: starts the game with settings.env.
    public void launchGame() {
        runOnUiThread(() -> startActivity(new Intent(this, GameActivity.class)));
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != PICK_DOCUMENT) {
            super.onActivityResult(request, result, data);
            return;
        }
        String path = "", name = "";
        Uri uri = data != null ? data.getData() : null;
        if (result == RESULT_OK && uri != null) {
            // The launcher reads the document through the descriptor, which
            // stays open for the rest of the run: /proc/self/fd/N.
            try {
                ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(uri, "r");
                if (descriptor != null) path = "/proc/self/fd/" + descriptor.detachFd();
            } catch (Exception error) {
                Log.w("FreeRiders", "cannot open " + uri, error);
            }
            try (Cursor cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME },
                                                            null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) name = cursor.getString(0);
            } catch (Exception error) {
                name = uri.getLastPathSegment();
            }
        }
        nativeDocumentPicked(path, name != null ? name : "");
    }

    // Called from the launcher's thread: the system font's glyphs for these
    // characters at a pixel size (the launcher's own rasterizer cannot read
    // Android's variable CJK fonts). Per glyph, little-endian: width, height
    // (ints), advance, x and y offset from the line's top (floats), then
    // width * height coverage bytes.
    public byte[] renderGlyphs(int[] codepoints, float size, boolean bold) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(bold ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        paint.setTextSize(size);
        paint.setColor(0xFFFFFFFF);
        Paint.FontMetrics metrics = paint.getFontMetrics();
        int total = 0;
        Bitmap[] bitmaps = new Bitmap[codepoints.length];
        Rect[] bounds = new Rect[codepoints.length];
        float[] advances = new float[codepoints.length];
        for (int i = 0; i < codepoints.length; ++i) {
            String text = new String(Character.toChars(codepoints[i]));
            Rect rect = new Rect();
            paint.getTextBounds(text, 0, text.length(), rect);
            advances[i] = paint.measureText(text);
            int width = Math.max(rect.width() + 2, 1), height = Math.max(rect.height() + 2, 1);
            Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ALPHA_8);
            new Canvas(bitmap).drawText(text, 1 - rect.left, 1 - rect.top, paint);
            bitmaps[i] = bitmap;
            bounds[i] = rect;
            total += 20 + width * height;
        }
        ByteBuffer out = ByteBuffer.allocate(total).order(ByteOrder.LITTLE_ENDIAN);
        for (int i = 0; i < codepoints.length; ++i) {
            Bitmap bitmap = bitmaps[i];
            int width = bitmap.getWidth(), height = bitmap.getHeight();
            out.putInt(width).putInt(height).putFloat(advances[i]);
            out.putFloat(bounds[i].left - 1).putFloat(bounds[i].top - 1 - metrics.ascent);
            ByteBuffer pixels = ByteBuffer.allocate(bitmap.getRowBytes() * height);
            bitmap.copyPixelsToBuffer(pixels);
            byte[] rows = pixels.array();
            for (int y = 0; y < height; ++y) out.put(rows, y * bitmap.getRowBytes(), width);
            bitmap.recycle();
        }
        return out.array();
    }

    private static native void nativeDocumentPicked(String path, String name);
}
