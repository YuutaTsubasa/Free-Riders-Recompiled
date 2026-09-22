"""Synthetic XDVDFS fixtures assembled from documented on-disc fields."""
import hashlib
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from test_xex import make_xex
try:
    from freeriders import disc
except ImportError:
    disc = None

SECTOR = 2048
MARKER = b"MICROSOFT*XBOX*MEDIA"


def node(name, sector=40, size=3, directory=False, left=0, right=0):
    encoded = name.encode("latin1")
    return struct.pack("<HHIIBB", left, right, sector, size, 0x10 if directory else 0, len(encoded)) + encoded


def make_disc(nodes=None, child=None):
    data = bytearray(SECTOR * 44)
    descriptor = SECTOR * 32
    data[descriptor:descriptor + 20] = MARKER
    data[descriptor + SECTOR - 20:descriptor + SECTOR] = MARKER
    root = nodes if nodes is not None else node("default.xex", 40, 512, right=16) + bytes(39) + node("data", 36, 128, True)
    struct.pack_into("<II", data, descriptor + 20, 34, len(root))
    data[34 * SECTOR:34 * SECTOR + len(root)] = root
    child = child if child is not None else node("hello.bin", 41, 3)
    data[36 * SECTOR:36 * SECTOR + len(child)] = child
    data[40 * SECTOR:40 * SECTOR + 512] = make_xex()
    data[41 * SECTOR:41 * SECTOR + 3] = b"abc"
    return data


class DiscTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(disc, "Disc reader must be implemented")
        scratch = Path(__file__).resolve().parents[1] / "out" / "tests"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.iso = self.root / "fixture.iso"

    def open_disc(self, data=None):
        self.iso.write_bytes(make_disc() if data is None else data)
        image = disc.DiscImage(self.iso)
        self.addCleanup(image.close)
        return image

    def extract(self, image, **kwargs):
        return image.extract(self.root / "new", expected_sha256=hashlib.sha256(make_xex()).hexdigest(),
                             expected_title_id=0x5345084D, **kwargs)

    def test_recursive_listing_and_reads(self):
        image = self.open_disc()
        self.assertEqual({entry.path for entry in image.entries}, {"default.xex", "data", "data/hello.bin"})
        self.assertEqual(image.partition_offset, 0)
        self.assertEqual(image.read_file("data/hello.bin"), b"abc")

    def test_supported_partition_bases(self):
        for base in (0xFD90000, 0x2080000):
            with self.subTest(base=base):
                with self.iso.open("wb") as file:
                    file.seek(base)
                    file.write(make_disc())
                with disc.DiscImage(self.iso) as image:
                    self.assertEqual(image.partition_offset, base)
                    self.assertEqual(image.read_file("data/hello.bin"), b"abc")

    def test_bad_marker(self):
        data = make_disc()
        data[32 * SECTOR] = 0
        with self.assertRaises(ValueError):
            self.open_disc(data)

    def test_bad_closing_marker(self):
        data = make_disc()
        data[33 * SECTOR - 1] = 0
        with self.assertRaises(ValueError):
            self.open_disc(data)

    def test_root_extent_out_of_bounds(self):
        data = make_disc()
        struct.pack_into("<II", data, 32 * SECTOR + 20, 0xFFFFFFFF, 4096)
        with self.assertRaises(ValueError):
            self.open_disc(data)

    def test_file_extent_out_of_bounds(self):
        with self.assertRaises(ValueError):
            self.open_disc(make_disc(node("bad", 43, 4096)))

    def test_node_pointer_out_of_bounds(self):
        with self.assertRaises(ValueError):
            self.open_disc(make_disc(node("bad", right=65535)))

    def test_node_cycle(self):
        root = node("a", right=8) + bytes(17) + node("b", right=8)
        with self.assertRaises(ValueError):
            self.open_disc(make_disc(root))

    def test_overlapping_nodes(self):
        with self.assertRaises(ValueError):
            self.open_disc(make_disc(node("bad", right=1)))

    def test_directory_cycle(self):
        with self.assertRaises(ValueError):
            self.open_disc(make_disc(node("again", 34, 64, True)))

    def test_truncated_name(self):
        with self.assertRaises(ValueError):
            self.open_disc(make_disc(node("bad")[:-1]))

    def test_unsafe_names(self):
        for name in ("", ".", "..", "../bad", "a/b", "a\\b", "C:bad", "NUL", "con.txt", "COM1", "LPT9.txt",
                     "CONIN$", "CONOUT$", "bad.", "bad ", "a\x00b", "a?b", "a*b", "a<b", "a|b", "a\"b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.open_disc(make_disc(node(name)))

    def test_reserved_device_with_space_before_extension(self):
        for name in ("NUL .txt", "CON .bin", "AUX  .data", "COM1 .dat"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.open_disc(make_disc(node(name)))

    def test_case_collision(self):
        for second in ("name", "NAME"):
            root = node("name", right=8) + bytes(14) + node(second)
            with self.subTest(second=second), self.assertRaises(ValueError):
                self.open_disc(make_disc(root))

    def test_read_is_bounded(self):
        image = self.open_disc()
        with self.assertRaises(ValueError):
            image.read_file("default.xex", max_bytes=10)

    def test_extract_full_disc_and_manifest(self):
        manifest = self.extract(self.open_disc())
        destination = self.root / "new"
        self.assertEqual((destination / "data/hello.bin").read_bytes(), b"abc")
        self.assertTrue(manifest["complete"])
        self.assertEqual(json.loads((destination / ".source-manifest.json").read_text()), manifest)
        self.assertEqual({entry["path"] for entry in manifest["files"]}, {"data/hello.bin", "default.xex"})
        entry = next(e for e in manifest["files"] if e["path"] == "data/hello.bin")
        self.assertEqual(entry["sha256"], hashlib.sha256(b"abc").hexdigest())

    def test_extract_selected_file(self):
        self.extract(self.open_disc(), paths=["default.xex"])
        self.assertTrue((self.root / "new/default.xex").is_file())
        self.assertFalse((self.root / "new/data").exists())

    def test_extract_refuses_existing_destination(self):
        (self.root / "new").mkdir()
        with self.assertRaises(FileExistsError):
            self.extract(self.open_disc())
        self.assertEqual(list((self.root / "new").iterdir()), [])

    def test_extract_verifies_xex_digest_and_title(self):
        image = self.open_disc()
        for digest, title in (("0" * 64, 0x5345084D), (hashlib.sha256(make_xex()).hexdigest(), 1)):
            with self.subTest(digest=digest, title=title), self.assertRaises(ValueError):
                image.extract(self.root / "new", expected_sha256=digest, expected_title_id=title)
            self.assertFalse((self.root / "new").exists())

    def test_extract_failure_does_not_publish(self):
        image = self.open_disc()
        original = image._copy_file
        def fail_on_second(entry, output):
            if entry.path == "data/hello.bin":
                raise OSError("simulated media failure")
            return original(entry, output)
        with patch.object(image, "_copy_file", side_effect=fail_on_second), self.assertRaises(OSError):
            self.extract(image)
        self.assertFalse((self.root / "new").exists())
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["fixture.iso"])

    def test_reject_symlink_parent(self):
        real = self.root / "real"
        real.mkdir()
        link = self.root / "link"
        try:
            link.symlink_to(real, target_is_directory=True)
        except OSError:
            self.skipTest("Creating symlinks requires privileges on this host")
        with self.assertRaises(ValueError):
            self.open_disc().extract(link / "new")
        self.assertFalse((real / "new").exists())

    def test_extract_unknown_path_does_not_publish(self):
        with self.assertRaises(ValueError):
            self.extract(self.open_disc(), paths=["../outside"])
        self.assertFalse((self.root / "new").exists())

    def test_cli_cannot_overwrite_source_image(self):
        import rom_tool
        self.iso.write_bytes(make_disc())
        original = self.iso.read_bytes()
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            result = rom_tool.main(["inspect", "--iso", str(self.iso), "--output", str(self.iso)])
        self.assertEqual(result, 1)
        self.assertEqual(self.iso.read_bytes(), original)

    def test_cli_cannot_overwrite_source_xex(self):
        import rom_tool
        source = self.root / "source.xex"
        source.write_bytes(make_xex())
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            result = rom_tool.main(["xex", str(source), "--output", str(source)])
        self.assertEqual(result, 1)
        self.assertEqual(source.read_bytes(), make_xex())

    def test_cli_report(self):
        import rom_tool
        self.iso.write_bytes(make_disc())
        output = self.root / "report.json"
        with contextlib.redirect_stdout(io.StringIO()):
            result = rom_tool.main(["inspect", "--iso", str(self.iso), "--output", str(output)])
        self.assertEqual(result, 0)
        report = json.loads(output.read_text())
        self.assertEqual(report["file_count"], 2)
        self.assertFalse(report["supported_source"])

    def test_empty_directories_can_share_zero_extent(self):
        root = node("a", 0, 0, True, right=8) + bytes(17) + node("b", 0, 0, True)
        image = self.open_disc(make_disc(root))
        self.assertEqual({e.path for e in image.entries}, {"a", "b"})

    def test_overlapping_directory_extents_rejected_before_read(self):
        # The second sector lies inside the first table despite different starts.
        root = node("a", 36, SECTOR * 2, True, right=8) + bytes(17) + node("b", 37, SECTOR, True)
        data = make_disc(root, node("child", 41, 3))
        data[37 * SECTOR:37 * SECTOR + 19] = node("other", 41, 3)
        calls = []
        original = disc.DiscImage._read
        def record_read(image, offset, size):
            calls.append((offset, size))
            return original(image, offset, size)
        with patch.object(disc.DiscImage, "_read", new=record_read):
            with self.assertRaisesRegex(ValueError, "overlapping.*directory"):
                self.open_disc(data)
        self.assertFalse(any(offset in (36 * SECTOR, 37 * SECTOR) for offset, _ in calls))

    def test_cumulative_directory_budget_checked_before_read(self):
        root = node("a", 36, 128, True, right=8) + bytes(17) + node("b", 37, 128, True)
        data = make_disc(root, node("child", 41, 3))
        data[37 * SECTOR:37 * SECTOR + 19] = node("other", 41, 3)
        calls = []
        original = disc.DiscImage._read
        def record_read(image, offset, size):
            calls.append((offset, size))
            return original(image, offset, size)
        with patch.object(disc, "MAX_TOTAL_DIRECTORY_BYTES", len(root) + 128, create=True):
            with patch.object(disc.DiscImage, "_read", new=record_read):
                with self.assertRaisesRegex(ValueError, "cumulative directory.*resource limit"):
                    self.open_disc(data)
        self.assertFalse(any(offset in (36 * SECTOR, 37 * SECTOR) for offset, _ in calls))

    def test_directory_count_resource_budget(self):
        with patch.object(disc, "MAX_DIRECTORIES", 1, create=True):
            with self.assertRaisesRegex(ValueError, "directory count.*resource limit"):
                self.open_disc()


if __name__ == "__main__":
    unittest.main()
