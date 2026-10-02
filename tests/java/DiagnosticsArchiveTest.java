package com.freeriders.recompiled;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

public class DiagnosticsArchiveTest {
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    static void writeString(Path path, String text) throws IOException {
        Files.write(path, text.getBytes(StandardCharsets.UTF_8));
    }
    static Map<String, String> archive(File root) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DiagnosticsArchive.write(root, bytes, "model=test\nversion=test\n");
        Map<String, String> files = new HashMap<>();
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry e; (e = zip.getNextEntry()) != null;) {
                ByteArrayOutputStream entry = new ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                for (int count; (count = zip.read(buffer)) != -1;)
                    entry.write(buffer, 0, count);
                files.put(e.getName(), new String(entry.toByteArray(), StandardCharsets.UTF_8));
            }
        }
        return files;
    }
    public static void main(String[] args) throws Exception {
        File root = new File(args[0], "input");
        check(root.mkdir(), "test directory");
        Map<String, String> empty = archive(root);
        check(empty.get("report.txt").contains("game.log: missing"), "missing log explanation");
        check(empty.get("device.txt").contains("model=test"), "device metadata");
        writeString(new File(root, "game.log").toPath(), "GPU header\nframe timing\n");
        writeString(new File(root, "settings.env").toPath(),
                "SFR_RENDER_SCALE=0.5\nSFR_GRAPHICS=vulkan\nSFR_CAMERA_DEVICE=private-device\nSFR_AVATAR_MODEL=/private/model\n");
        writeString(new File(root, "debug.env").toPath(), "SFR_FRAME_METRICS=1\nSECRET=private\n");
        writeString(new File(root, "save.bin").toPath(), "private-save");
        Map<String, String> small = archive(root);
        check(small.get("game.log").equals("GPU header\nframe timing\n"), "small log preserved");
        check(small.get("settings.env").equals("SFR_RENDER_SCALE=0.5\nSFR_GRAPHICS=vulkan\n"), "settings allowlist");
        check(small.get("debug.env").equals("SFR_FRAME_METRICS=1\n"), "overrides allowlist");
        check(!small.containsKey("save.bin"), "no recursive export");
        try (RandomAccessFile log = new RandomAccessFile(new File(root, "game.log"), "rw")) {
            log.setLength(16 * 1024 * 1024);
            log.seek(log.length() - 5);
            log.writeBytes("TAIL\n");
        }
        String large = archive(root).get("game.log");
        check(large.startsWith("GPU header\n"), "keep GPU header");
        check(large.endsWith("TAIL\n"), "keep recent frames");
        check(large.length() < 5 * 1024 * 1024, "bounded export");
        check(large.contains("LOG MIDDLE OMITTED"), "truncation marker");
        boolean failed = false;
        try {
            DiagnosticsArchive.write(root, new OutputStream() {
                public void write(int value) throws IOException { throw new IOException("disk full"); }
            }, "test");
        } catch (IOException expected) { failed = true; }
        check(failed, "write failure must reach UI");
        System.out.println("Android diagnostics archive tests passed");
    }
}
