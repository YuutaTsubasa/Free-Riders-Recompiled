"""Bounded metadata reader for Xbox 360 XEX2 executables (no decryption)."""
import struct

MAX_XEX_BYTES = 64 * 1024 * 1024
# Explicit object/work budgets, independent of the input byte-size limit.
# The supported image has 16 optional headers, 21 static libraries, and
# 2 import libraries with 611 total records.
MAX_OPTIONAL_HEADERS = 1024
MAX_STATIC_LIBRARIES = 1024
MAX_IMPORT_LIBRARIES = 256
MAX_IMPORT_RECORDS = 65536
MAX_IMPORT_STRING_BYTES = 64 * 1024


def parse_xex(data):
    """Return JSON-ready metadata, rejecting offsets outside the XEX header."""
    data = memoryview(data)
    if len(data) < 24 or len(data) > MAX_XEX_BYTES:
        raise ValueError("XEX size is outside supported bounds")
    magic, flags, pe_offset, reserved, security_offset, count = struct.unpack_from(">4s5I", data)
    if magic != b"XEX2":
        raise ValueError("Expected XEX2 magic")
    if count > MAX_OPTIONAL_HEADERS:
        raise ValueError("XEX optional header resource limit exceeded")
    table_end = 24 + count * 8
    if table_end > pe_offset or pe_offset >= len(data):
        raise ValueError("XEX optional table or PE data offset is out of bounds")

    def region(offset, size, label):
        if offset < table_end or size < 0 or offset > pe_offset or size > pe_offset - offset:
            raise ValueError(f"XEX {label} is out of header bounds")
        return data[offset:offset + size]

    region(security_offset, 8, "security header")
    security_size, image_size = struct.unpack_from(">II", data, security_offset)
    if security_size < 8:
        raise ValueError("XEX security header is too short")
    region(security_offset, security_size, "security header")
    result = {"magic": "XEX2", "module_flags": flags, "pe_data_offset": pe_offset,
              "security_info_offset": security_offset, "security_info_size": security_size,
              "image_size": image_size, "optional_headers": []}
    seen = set()
    for index in range(count):
        key, value = struct.unpack_from(">II", data, 24 + index * 8)
        if key in seen:
            raise ValueError(f"Duplicate XEX optional header {key:#x}")
        seen.add(key)
        words = key & 0xFF
        payload = None
        if words == 0xFF:
            region(value, 4, "variable optional header")
            size = struct.unpack_from(">I", data, value)[0]
            if size < 4:
                raise ValueError("XEX variable header size is too small")
            payload = region(value, size, "variable optional header")
        elif words > 1:
            payload = region(value, words * 4, "fixed optional header")
        record = {"key": f"0x{key:08X}", "value": value}
        if payload is not None:
            record["size"] = len(payload)
        result["optional_headers"].append(record)
        if key == 0x10100:
            result["entry_point"] = value
        elif key == 0x10201:
            result["image_base"] = value
        elif key == 0x40006:
            media, version, base_version, title, platform, executable_type, disc_number, disc_count, save_id = struct.unpack_from(">IIII4BI", payload)
            result["execution_info"] = {"media_id": media, "version": version, "base_version": base_version,
                                        "title_id": title, "platform": platform, "executable_type": executable_type,
                                        "disc_number": disc_number, "disc_count": disc_count, "savegame_id": save_id}
        elif key == 0x3FF:
            if len(payload) < 8:
                raise ValueError("XEX file format header is truncated")
            encryption, compression = struct.unpack_from(">HH", payload, 4)
            result["file_format"] = {"encryption": encryption, "compression": compression}
            if compression == 1 and (len(payload) - 8) % 8:
                raise ValueError("XEX basic compression block table is truncated")
            if compression == 2:
                if len(payload) < 36:
                    raise ValueError("XEX normal compression header is truncated")
                window, block_size = struct.unpack_from(">II", payload, 8)
                result["file_format"].update(window_size=window, first_block_size=block_size)
        elif key == 0x103FF:
            result["import_libraries"] = _parse_imports(payload)
        elif key == 0x200FF:
            if (len(payload) - 4) % 16:
                raise ValueError("XEX static library table is truncated")
            if (len(payload) - 4) // 16 > MAX_STATIC_LIBRARIES:
                raise ValueError("XEX static library resource limit exceeded")
            result["static_libraries"] = []
            for offset in range(4, len(payload), 16):
                name, major, minor, build, qfe = struct.unpack_from(">8s4H", payload, offset)
                result["static_libraries"].append({"name": name.rstrip(b"\0").decode("ascii", errors="replace"),
                                                   "major": major, "minor": minor, "build": build, "qfe": qfe})
    return result


def _parse_imports(payload):
    if len(payload) < 12:
        raise ValueError("XEX import header is truncated")
    _, string_size, count = struct.unpack_from(">III", payload)
    if count > MAX_IMPORT_LIBRARIES:
        raise ValueError("XEX import library resource limit exceeded")
    if string_size > MAX_IMPORT_STRING_BYTES:
        raise ValueError("XEX import string resource limit exceeded")
    if string_size > len(payload) - 12:
        raise ValueError("XEX import string table is out of bounds")
    strings = bytes(payload[12:12 + string_size])
    names = [name.decode("ascii", errors="replace") for name in strings.split(b"\0") if name]
    if strings and strings[-1] != 0:
        raise ValueError("XEX import string is unterminated")
    offset = 12 + string_size
    if count > (len(payload) - offset) // 40:
        raise ValueError("XEX import library count is out of bounds")
    libraries = []
    total_records = 0
    for _ in range(count):
        if offset + 40 > len(payload):
            raise ValueError("XEX import library is truncated")
        size = struct.unpack_from(">I", payload, offset)[0]
        library_id, version, minimum, name_index, record_count = struct.unpack_from(">IIIHH", payload, offset + 24)
        if size < 40 or size > len(payload) - offset or record_count * 4 != size - 40:
            raise ValueError("XEX import records are out of bounds")
        total_records += record_count
        if total_records > MAX_IMPORT_RECORDS:
            raise ValueError("XEX import record resource limit exceeded")
        if name_index >= len(names):
            raise ValueError("XEX import name index is out of bounds")
        libraries.append({"name": names[name_index], "id": library_id, "version": version,
                          "minimum_version": minimum, "record_count": record_count})
        offset += size
    if offset != len(payload):
        raise ValueError("XEX import table has trailing data")
    return libraries
