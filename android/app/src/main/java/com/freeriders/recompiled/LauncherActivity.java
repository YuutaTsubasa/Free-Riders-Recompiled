package com.freeriders.recompiled;

import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.libsdl.app.SDLActivity;

// The app's entry: the launcher (liblauncher.so, src/launcher_main.cpp) with
// its settings and install pages. The game runs in GameActivity, in a process
// of its own (android:process=":game"), because the runtime reads its
// settings while its library loads.
public class LauncherActivity extends SDLActivity {
    private static final int PICK_DOCUMENT = 1;
    private static final int EXPORT_DIAGNOSTICS = 2;
    private boolean exportingDiagnostics;

    // Called by SDL; picker state is owned by the Android UI thread.
    public void exportDiagnostics() {
        runOnUiThread(() -> {
            if (exportingDiagnostics) return;
            exportingDiagnostics = true;
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
            String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.ROOT)
                    .format(new java.util.Date());
            intent.putExtra(Intent.EXTRA_TITLE, "FreeRiders-diagnostics-" + stamp + ".zip");
            try {
                startActivityForResult(intent, EXPORT_DIAGNOSTICS);
            } catch (Exception error) {
                exportingDiagnostics = false;
                diagnosticStatus(false);
                Log.w("FreeRiders", "cannot open export picker", error);
            }
        });
    }

    private void diagnosticStatus(boolean success) {
        boolean chinese = getResources().getConfiguration().getLocales().get(0).getLanguage().equals("zh");
        String message = success ? (chinese ? "診斷 ZIP 已儲存" : "Diagnostic ZIP saved")
                                 : (chinese ? "無法儲存診斷 ZIP，請重試" : "Could not save diagnostic ZIP. Please retry.");
        android.widget.Toast.makeText(this, message, android.widget.Toast.LENGTH_LONG).show();
    }

    // How the app's processes (the launcher and :game) last ended, newest
    // first, with a native crash's tombstone or an ANR's trace.
    private java.util.List<DiagnosticsArchive.Exit> recentExits() {
        java.util.List<DiagnosticsArchive.Exit> exits = new java.util.ArrayList<>();
        if (android.os.Build.VERSION.SDK_INT < 30) return exits;
        try {
            android.app.ActivityManager manager = getSystemService(android.app.ActivityManager.class);
            for (android.app.ApplicationExitInfo info : manager.getHistoricalProcessExitReasons(getPackageName(), 0, 5)) {
                String summary = info.getProcessName() + " reason=" + info.getReason() + " status=" + info.getStatus()
                        + " time=" + new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT)
                                .format(new java.util.Date(info.getTimestamp()))
                        + " importance=" + info.getImportance() + " description=" + info.getDescription();
                byte[] trace = null;
                String traceName = null;
                final int reason = info.getReason();
                if (reason == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE
                        || reason == android.app.ApplicationExitInfo.REASON_ANR) {
                    try (java.io.InputStream input = info.getTraceInputStream()) {
                        if (input != null) {
                            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                            byte[] buffer = new byte[16384];
                            for (int count; (count = input.read(buffer)) != -1 && bytes.size() < 4 * 1024 * 1024;)
                                bytes.write(buffer, 0, count);
                            trace = bytes.toByteArray();
                            traceName = reason == android.app.ApplicationExitInfo.REASON_ANR ? "anr.txt" : "tombstone.pb";
                        }
                    } catch (Exception error) {
                        summary += " trace_unreadable=" + error.getClass().getSimpleName();
                    }
                }
                exits.add(new DiagnosticsArchive.Exit(summary, trace, traceName));
            }
        } catch (Exception error) {
            Log.w("FreeRiders", "cannot read exit reasons", error);
        }
        return exits;
    }

    private void saveDiagnostics(Uri uri) {
        new Thread(() -> {
            boolean success = false;
            try (OutputStream output = getContentResolver().openOutputStream(uri, "wt")) {
                if (output == null) throw new java.io.IOException("No output stream");
                android.content.pm.PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
                String device = "version=" + info.versionName + "\nversion_code=" + info.getLongVersionCode()
                        + "\nmanufacturer=" + android.os.Build.MANUFACTURER + "\nmodel=" + android.os.Build.MODEL
                        + "\nandroid=" + android.os.Build.VERSION.RELEASE + "\nsdk=" + android.os.Build.VERSION.SDK_INT
                        + "\nabis=" + String.join(",", android.os.Build.SUPPORTED_ABIS) + "\n";
                DiagnosticsArchive.write(getExternalFilesDir(null), output, device, recentExits());
                success = true;
            } catch (Exception error) {
                success = false;
                Log.w("FreeRiders", "cannot export diagnostics", error);
            }
            final boolean saved = success;
            runOnUiThread(() -> {
                exportingDiagnostics = false;
                diagnosticStatus(saved);
            });
        }, "diagnostic-export").start();
    }

    @Override
    protected void onCreate(Bundle state) {
        copyBundledShaderPack();
        copyBundledPipelineManifest();
        copyBundledPoseModel();
        super.onCreate(state);
    }

    // The camera's motion input reads its models from pose/ beside the game's
    // files (src/pose_estimator_onnx.cpp: the MediaPipe pose and person
    // detection models). An APK that carries them (scripts/package_android.py
    // --pose) puts them there once per install.
    private void copyBundledPoseModel() {
        File directory = getExternalFilesDir(null);
        if (directory == null) return;
        File folder = new File(directory, "pose");
        File marker = new File(folder, "models.bundled");
        String version;
        String[] models;
        try {
            version = Long.toString(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
            models = getAssets().list("pose");
        } catch (Exception error) {
            return;
        }
        if (models == null || models.length == 0) return;  // an APK without them: the pad stays in charge
        String copied = "";
        try (InputStream in = new FileInputStream(marker)) {
            byte[] text = new byte[64];
            int length = Math.max(in.read(text), 0);
            copied = new String(text, 0, length, "UTF-8");
        } catch (Exception missing) {
        }
        if (copied.equals(version)) return;
        if (!folder.isDirectory() && !folder.mkdirs()) return;
        for (String name : models) {
            File model = new File(folder, name);
            File partial = new File(folder, name + ".partial");
            try (InputStream in = getAssets().open("pose/" + name); OutputStream out = new FileOutputStream(partial)) {
                byte[] buffer = new byte[1 << 16];
                for (int read; (read = in.read(buffer)) > 0; ) out.write(buffer, 0, read);
            } catch (Exception error) {
                partial.delete();
                return;
            }
            model.delete();
            if (!partial.renameTo(model)) return;
        }
        try (OutputStream out = new FileOutputStream(marker)) {
            out.write(version.getBytes("UTF-8"));
        } catch (Exception ignored) {
        }
    }

    // This small portable recipe list is release-owned. The separately learned
    // pipeline-cache/vulkan.manifest and the player's driver cache are retained.
    private void copyBundledPipelineManifest() {
        File directory = getExternalFilesDir(null);
        if (directory == null) return;
        File target = new File(directory, "pipelines-vulkan.manifest");
        File partial = new File(directory, "pipelines-vulkan.manifest.partial");
        try (InputStream in = getAssets().open("pipelines-vulkan.manifest");
             OutputStream out = new FileOutputStream(partial)) {
            byte[] buffer = new byte[16384];
            for (int n; (n = in.read(buffer)) > 0;) out.write(buffer, 0, n);
        } catch (Exception unavailable) {
            partial.delete();
            target.delete(); // APKs without a list must not retain an old bundled list.
            return;
        }
        if (!partial.renameTo(target)) partial.delete();
    }

    // A release APK carries shaders.pack (scripts/package_android.py --pack).
    // It is copied beside the launcher's files when there is none, or when
    // the one there came from an older APK; a pack the player chose stays.
    private void copyBundledShaderPack() {
        File directory = getExternalFilesDir(null);
        if (directory == null) return;
        File pack = new File(directory, "shaders.pack");
        File marker = new File(directory, "shaders.pack.bundled");
        String version;
        try {
            version = Long.toString(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
        } catch (Exception error) {
            return;
        }
        String copied = "";
        try (InputStream in = new FileInputStream(marker)) {
            byte[] text = new byte[64];
            int length = Math.max(in.read(text), 0);
            copied = new String(text, 0, length, "UTF-8");
        } catch (Exception missing) {
            // No marker: the pack came from a version that wrote none, so it
            // is one of ours from an older APK and may be out of date.
        }
        // "chosen" is the player's own pack (the launcher's Choose shaders.pack
        // writes it); otherwise the marker names the install it was copied for.
        if (pack.exists() && (copied.equals("chosen") || copied.equals(version))) return;
        File partial = new File(directory, "shaders.pack.partial");
        try (InputStream in = getAssets().open("shaders.pack"); OutputStream out = new FileOutputStream(partial)) {
            byte[] buffer = new byte[1 << 16];
            for (int read; (read = in.read(buffer)) > 0; ) out.write(buffer, 0, read);
        } catch (Exception error) {
            partial.delete();
            return;  // an APK without a bundled pack
        }
        if (!partial.renameTo(pack)) return;
        try (OutputStream out = new FileOutputStream(marker)) {
            out.write(version.getBytes("UTF-8"));
        } catch (Exception ignored) {
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL2", "launcher" };
    }

    // The launcher reads its settings while liblauncher.so loads, so debug.env
    // (the same NAME=VALUE lines GameActivity reads) is in the environment
    // first. Only diagnosis writes that file; SFR_LAUNCHER_AUTOPLAY=1 there
    // makes an unattended run possible, since this activity cannot be started
    // with an environment of its own.
    @Override
    public void loadLibraries() {
        File directory = getExternalFilesDir(null);
        if (directory != null) {
            File debug = new File(directory, "debug.env");
            if (debug.isFile()) {
                try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(debug))) {
                    for (String line; (line = reader.readLine()) != null;) {
                        line = line.trim();
                        int equals = line.indexOf('=');
                        if (line.isEmpty() || line.startsWith("#") || equals <= 0) continue;
                        try {
                            android.system.Os.setenv(line.substring(0, equals), line.substring(equals + 1), true);
                        } catch (android.system.ErrnoException error) {
                            Log.w("FreeRiders", "cannot set " + line, error);
                        }
                    }
                } catch (java.io.IOException error) {
                    Log.w("FreeRiders", "cannot read debug.env", error);
                }
            }
        }
        super.loadLibraries();
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
        if (request == EXPORT_DIAGNOSTICS) {
            Uri destination = data != null ? data.getData() : null;
            if (result == RESULT_OK && destination != null) saveDiagnostics(destination);
            else exportingDiagnostics = false;
            return;
        }
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
