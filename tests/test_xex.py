"""Small independently assembled XEX headers; no game data is used."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
try:
    from freeriders import xex
except ImportError:
    xex = None


def make_xex(title_id=0x5345084D):
    data = bytearray(512)
    struct.pack_into(">4s5I", data, 0, b"XEX2", 1, 256, 0, 128, 4)
    for index, pair in enumerate([(0x10100, 0x824D22F0), (0x10201, 0x82000000),
                                  (0x40006, 64), (0x3FF, 96)]):
        struct.pack_into(">II", data, 24 + index * 8, *pair)
    struct.pack_into(">IIII4BI", data, 64, 0, 1, 1, title_id, 2, 0, 1, 1, 0)
    struct.pack_into(">IHH", data, 96, 8, 0, 0)
    struct.pack_into(">II", data, 128, 128, 256)
    return data


def with_variable_header(key, payload):
    """Replace only file-format record and move PE/security to provide space."""
    data = make_xex()
    struct.pack_into(">I", data, 8, 384)
    struct.pack_into(">I", data, 16, 320)
    struct.pack_into(">II", data, 320, 64, 256)
    struct.pack_into(">II", data, 48, key, 96)
    data[96:96 + len(payload)] = payload
    return data


def import_payload():
    name = b"synthetic.xex\0\0\0"
    library = struct.pack(">I20sIIIHHI", 44, bytes(20), 7, 1, 1, 0, 1, 0x82001000)
    return bytearray(struct.pack(">III", 12 + len(name) + len(library), len(name), 1) + name + library)


def variable_xex(key, payload):
    """One optional payload with independent valid security/header extents."""
    pe_offset = 40 + len(payload)
    data = bytearray(pe_offset + 1)
    struct.pack_into(">4s5I", data, 0, b"XEX2", 1, pe_offset, 0, 32, 1)
    struct.pack_into(">II", data, 24, key, 40)
    struct.pack_into(">II", data, 32, 8, 256)
    data[40:pe_offset] = payload
    return data


class XexTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(xex, "XEX parser must be implemented")

    def test_metadata(self):
        result = xex.parse_xex(make_xex())
        self.assertEqual(result["entry_point"], 0x824D22F0)
        self.assertEqual(result["image_base"], 0x82000000)
        self.assertEqual(result["execution_info"]["title_id"], 0x5345084D)
        self.assertEqual(result["file_format"]["compression"], 0)
        self.assertEqual(result["pe_data_offset"], 256)

    def test_truncated_fixed_header(self):
        for size in range(24):
            with self.subTest(size=size), self.assertRaises(ValueError):
                xex.parse_xex(make_xex()[:size])

    def test_wrong_magic(self):
        data = make_xex()
        data[:4] = b"MZ00"
        with self.assertRaises(ValueError):
            xex.parse_xex(data)

    def test_invalid_optional_count(self):
        data = make_xex()
        struct.pack_into(">I", data, 20, 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            xex.parse_xex(data)

    def test_optional_payload_out_of_bounds(self):
        for offset in (0, 28, 240, 0xFFFFFFFC):
            data = make_xex()
            struct.pack_into(">I", data, 44, offset)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                xex.parse_xex(data)

    def test_variable_size_out_of_bounds(self):
        for size in (0, 4, 4096):
            data = make_xex()
            struct.pack_into(">I", data, 96, size)
            with self.subTest(size=size), self.assertRaises(ValueError):
                xex.parse_xex(data)

    def test_security_bounds(self):
        for offset in (0, 24, 250, 1000):
            data = make_xex()
            struct.pack_into(">I", data, 16, offset)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                xex.parse_xex(data)

    def test_pe_offset_bounds(self):
        for offset in (0, 24, 1024):
            data = make_xex()
            struct.pack_into(">I", data, 8, offset)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                xex.parse_xex(data)

    def test_duplicate_optional_key(self):
        data = make_xex()
        struct.pack_into(">I", data, 32, 0x10100)
        with self.assertRaises(ValueError):
            xex.parse_xex(data)

    def test_truncated_execution_info(self):
        with self.assertRaises(ValueError):
            xex.parse_xex(make_xex()[:80])

    def test_unknown_variable_header_still_validates_bounds(self):
        data = make_xex()
        struct.pack_into(">II", data, 48, 0x7777FF, 0xFFFFFFFC)
        with self.assertRaises(ValueError):
            xex.parse_xex(data)

    def test_import_library_metadata(self):
        result = xex.parse_xex(with_variable_header(0x103FF, import_payload()))
        self.assertEqual(result["import_libraries"], [{"name": "synthetic.xex", "id": 7, "version": 1,
                                                    "minimum_version": 1, "record_count": 1}])

    def test_import_string_table_bounds(self):
        payload = import_payload()
        struct.pack_into(">I", payload, 4, 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            xex.parse_xex(with_variable_header(0x103FF, payload))

    def test_import_library_count_bounds(self):
        payload = import_payload()
        struct.pack_into(">I", payload, 8, 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            xex.parse_xex(with_variable_header(0x103FF, payload))

    def test_import_record_bounds(self):
        payload = import_payload()
        library_offset = 12 + struct.unpack_from(">I", payload, 4)[0]
        struct.pack_into(">H", payload, library_offset + 38, 0xFFFF)
        with self.assertRaises(ValueError):
            xex.parse_xex(with_variable_header(0x103FF, payload))

    def test_import_name_index_bounds(self):
        payload = import_payload()
        library_offset = 12 + struct.unpack_from(">I", payload, 4)[0]
        struct.pack_into(">H", payload, library_offset + 36, 2)
        with self.assertRaises(ValueError):
            xex.parse_xex(with_variable_header(0x103FF, payload))

    def test_static_library_metadata(self):
        payload = struct.pack(">I8s4H", 20, b"FAKELIB", 2, 0, 1234, 1)
        result = xex.parse_xex(with_variable_header(0x200FF, payload))
        self.assertEqual(result["static_libraries"][0]["name"], "FAKELIB")
        self.assertEqual(result["static_libraries"][0]["build"], 1234)

    def test_static_library_bounds(self):
        payload = struct.pack(">I", 19) + bytes(15)
        with self.assertRaises(ValueError):
            xex.parse_xex(with_variable_header(0x200FF, payload))

    def test_compression_table_bounds(self):
        for payload in (struct.pack(">IHH", 8, 1, 2), struct.pack(">IHHI", 12, 1, 1, 1)):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                xex.parse_xex(with_variable_header(0x3FF, payload))

    def test_optional_header_resource_budget(self):
        count = 1025
        security_offset = 24 + count * 8
        data = bytearray(security_offset + 9)
        struct.pack_into(">4s5I", data, 0, b"XEX2", 1, security_offset + 8, 0, security_offset, count)
        for index in range(count):
            struct.pack_into(">II", data, 24 + index * 8, (index + 0x8000) << 8, 0)
        struct.pack_into(">II", data, security_offset, 8, 256)
        with self.assertRaisesRegex(ValueError, "optional.*resource limit"):
            xex.parse_xex(data)

    def test_static_library_resource_budget(self):
        count = 1025
        payload = struct.pack(">I", 4 + count * 16) + struct.pack(">8s4H", b"FAKELIB", 2, 0, 1234, 1) * count
        with self.assertRaisesRegex(ValueError, "static.*resource limit"):
            xex.parse_xex(variable_xex(0x200FF, payload))

    def test_import_library_resource_budget(self):
        count = 257
        library = struct.pack(">I20sIIIHH", 40, bytes(20), 7, 1, 1, 0, 0)
        payload = struct.pack(">III", 16 + count * 40, 4, count) + b"lib\0" + library * count
        with self.assertRaisesRegex(ValueError, "import librar.*resource limit"):
            xex.parse_xex(variable_xex(0x103FF, payload))

    def test_import_record_resource_budget_is_cumulative(self):
        records = 32769
        size = 40 + records * 4
        library = struct.pack(">I20sIIIHH", size, bytes(20), 7, 1, 1, 0, records) + bytes(records * 4)
        payload = struct.pack(">III", 16 + 2 * size, 4, 2) + b"lib\0" + library * 2
        with self.assertRaisesRegex(ValueError, "import record.*resource limit"):
            xex.parse_xex(variable_xex(0x103FF, payload))

    def test_import_string_resource_budget_before_splitting(self):
        strings = b"x\0" * 32769
        payload = struct.pack(">III", 12 + len(strings), len(strings), 0) + strings
        with self.assertRaisesRegex(ValueError, "import string.*resource limit"):
            xex.parse_xex(variable_xex(0x103FF, payload))


if __name__ == "__main__":
    unittest.main()
