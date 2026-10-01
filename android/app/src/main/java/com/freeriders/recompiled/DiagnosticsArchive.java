package com.freeriders.recompiled;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Bounded, explicit file selection; usable without the Android framework. */
final class DiagnosticsArchive {
    private static final int LOG_LIMIT = 4 * 1024 * 1024;
    private static final int LOG_HEAD = 256 * 1024;
    private static final Set<String> SETTINGS = new HashSet<>(Arrays.asList(
        "SFR_RENDER_SCALE", "SFR_WINDOW_WIDTH", "SFR_WINDOW_HEIGHT", "SFR_GRAPHICS",
        "SFR_VSYNC", "SFR_FRAME_LIMIT", "SFR_RENDER_EVERY", "SFR_PARALLEL_WORKER",
        "SFR_VERTEX_CACHE", "SFR_GPU_PIPELINE", "SFR_SKIP_MOVIES", "SFR_AUDIO",
        "SFR_FRAME_METRICS", "SFR_TRACE_GRAPHICS", "SFR_DIAGNOSTIC_ENTRIES", "SFR_TRACE_IMPORTS"));

    static void write(File root, OutputStream output, String device) throws IOException {
        if (root == null) throw new IOException("App storage unavailable");
        StringBuilder report = new StringBuilder(
            "Free Riders diagnostics\nLog may contain local file paths. Review before sharing.\n"
            + "settings.env: selected values from last launch; debug.env: selected overrides.\n"
            + "Timing summaries measure guest frame submissions, not display refreshes.\n");
        try (ZipOutputStream zip = new ZipOutputStream(output)) {
            text(zip, "device.txt", device);
            File log = new File(root, "game.log");
            if (log.isFile()) {
                try (RandomAccessFile input = new RandomAccessFile(log, "r")) {
                    long size = input.length();
                    report.append("game.log: ").append(size).append(" source bytes; modified_ms=")
                          .append(log.lastModified()).append('\n');
                    zip.putNextEntry(new ZipEntry("game.log"));
                    if (size <= LOG_LIMIT) copy(input, zip, size);
                    else {
                        copy(input, zip, LOG_HEAD);
                        zip.write("\n[LOG MIDDLE OMITTED: keeping header and recent frames]\n".getBytes(StandardCharsets.UTF_8));
                        input.seek(size - (LOG_LIMIT - LOG_HEAD));
                        copy(input, zip, LOG_LIMIT - LOG_HEAD);
                    }
                    zip.closeEntry();
                }
            } else report.append("game.log: missing; launch the game before exporting.\n");
            for (String name : new String[] {"settings.env", "debug.env"}) {
                File source = new File(root, name);
                if (!source.isFile()) { report.append(name).append(": missing\n"); continue; }
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                try (RandomAccessFile input = new RandomAccessFile(source, "r")) {
                    copy(input, bytes, 65536);
                }
                StringBuilder selected = new StringBuilder();
                for (String line : new String(bytes.toByteArray(), StandardCharsets.UTF_8).split("\n")) {
                    line = line.trim();
                    int equals = line.indexOf('=');
                    if (equals > 0 && SETTINGS.contains(line.substring(0, equals)))
                        selected.append(line).append('\n');
                }
                text(zip, name, selected.toString());
            }
            text(zip, "report.txt", report.toString());
        }
    }

    private static void copy(RandomAccessFile input, OutputStream out, long remaining) throws IOException {
        byte[] buffer = new byte[16384];
        while (remaining > 0) {
            int read = input.read(buffer, 0, (int)Math.min(remaining, buffer.length));
            if (read < 0) break;
            out.write(buffer, 0, read);
            remaining -= read;
        }
    }

    private static void text(ZipOutputStream zip, String name, String text) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(text.getBytes(StandardCharsets.UTF_8));
        zip.closeEntry();
    }
}
