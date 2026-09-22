"""Read-only, bounded XDVDFS intake and staged local extraction."""
from dataclasses import asdict, dataclass
from bisect import bisect_left
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import tempfile

from .xex import MAX_XEX_BYTES, parse_xex

SECTOR_SIZE = 2048
PARTITION_BASES = (0, 0xFD90000, 0x2080000)
MARKER = b"MICROSOFT*XBOX*MEDIA"
MAX_DIRECTORY_BYTES = 64 * 1024 * 1024
MAX_TOTAL_DIRECTORY_BYTES = 64 * 1024 * 1024
# Also bounds the interval index and its sorted insertion work.
MAX_DIRECTORIES = 4096
MAX_ENTRIES = 1_000_000
MAX_DEPTH = 256
MANIFEST_NAME = ".source-manifest.json"
EXPECTED_XEX_SHA256 = "3d58636bec9b92948f7dbe423c0aa96753349ba014194c4e62d2f809c14643a7"
EXPECTED_TITLE_ID = 0x5345084D


@dataclass(frozen=True)
class DiscEntry:
    path: str
    sector: int
    size: int
    is_directory: bool


def _validate_name(name):
    stem = name.split(".", 1)[0].rstrip(" ").upper()
    reserved = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}
    reserved.update(f"{prefix}{digit}" for prefix in ("COM", "LPT") for digit in "123456789¹²³")
    if (not name or name in (".", "..") or name[-1] in " ." or stem in reserved
            or any(ord(char) < 32 or char in '/\\:*?"<>|' for char in name)):
        raise ValueError(f"Unsafe disc path component: {name!r}")


def _safe_ancestors(path):
    """Reject symlinks and Windows junction/reparse points without resolving them."""
    for candidate in (path, *path.parents):
        try:
            info = candidate.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Output path uses a symlink or reparse point: {candidate}")
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError(f"Output ancestor is not a directory: {candidate}")


class DiscImage:
    def __init__(self, path):
        self.path = Path(path)
        self._file = self.path.open("rb")
        self.size = os.fstat(self._file.fileno()).st_size
        try:
            self.partition_offset, descriptor = self._find_partition()
            root_sector, root_size = struct.unpack_from("<II", descriptor, 20)
            self.entries = self._list_entries(root_sector, root_size)
            self._by_path = {entry.path: entry for entry in self.entries}
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self._file.close()

    def _read(self, offset, size):
        if offset < 0 or size < 0 or offset > self.size or size > self.size - offset:
            raise ValueError("Disc read is out of bounds")
        self._file.seek(offset)
        data = self._file.read(size)
        if len(data) != size:
            raise ValueError("Disc read was truncated")
        return data

    def _find_partition(self):
        for base in PARTITION_BASES:
            offset = base + 32 * SECTOR_SIZE
            if offset + SECTOR_SIZE > self.size:
                continue
            descriptor = self._read(offset, SECTOR_SIZE)
            if descriptor[:20] == MARKER:
                if descriptor[-20:] != MARKER:
                    raise ValueError("XDVDFS descriptor closing marker is invalid")
                return base, descriptor
        raise ValueError("No supported XDVDFS volume descriptor found")

    def _extent(self, sector, size):
        offset = self.partition_offset + sector * SECTOR_SIZE
        if size < 0 or offset > self.size or size > self.size - offset:
            raise ValueError("XDVDFS extent is out of bounds")
        return offset

    def _list_entries(self, root_sector, root_size):
        directories = []
        directory_extents = []
        total_directory_bytes = 0
        directory_count = 0

        def enqueue_directory(parent, sector, size, depth):
            nonlocal total_directory_bytes, directory_count
            start = self._extent(sector, size)
            if size > MAX_DIRECTORY_BYTES or depth > MAX_DEPTH:
                raise ValueError("XDVDFS directory resource limit exceeded")
            directory_count += 1
            if directory_count > MAX_DIRECTORIES:
                raise ValueError("XDVDFS directory count resource limit exceeded")
            if size > MAX_TOTAL_DIRECTORY_BYTES - total_directory_bytes:
                raise ValueError("XDVDFS cumulative directory byte resource limit exceeded")
            if not size:
                return
            end = start + size
            index = bisect_left(directory_extents, (start, end))
            if ((index and directory_extents[index - 1][1] > start)
                    or (index < len(directory_extents) and directory_extents[index][0] < end)):
                raise ValueError("Cyclic, shared or overlapping XDVDFS directory extent")
            directory_extents.insert(index, (start, end))
            total_directory_bytes += size
            directories.append((parent, sector, size, depth))

        # Reserve every discovered extent and its budget before reading it.
        enqueue_directory("", root_sector, root_size, 0)
        paths = set()
        result = []
        while directories:
            parent, sector, size, depth = directories.pop()
            table = self._read(self._extent(sector, size), size)
            pending = [0]
            occupied = bytearray(size)
            while pending:
                offset = pending.pop()
                if offset + 14 > size:
                    raise ValueError("XDVDFS node is out of bounds")
                left, right, file_sector, file_size, attributes, name_size = struct.unpack_from("<HHIIBB", table, offset)
                end = offset + 14 + name_size
                if end > size or any(occupied[offset:end]):
                    raise ValueError("XDVDFS node is truncated, overlapping or cyclic")
                occupied[offset:end] = b"\1" * (end - offset)
                name = table[offset + 14:end].decode("latin1")
                _validate_name(name)
                path = f"{parent}/{name}" if parent else name
                if path.casefold() in paths:
                    raise ValueError(f"Duplicate or case-colliding XDVDFS path: {path}")
                paths.add(path.casefold())
                is_directory = bool(attributes & 0x10)
                self._extent(file_sector, file_size)
                result.append(DiscEntry(path, file_sector, file_size, is_directory))
                if len(result) > MAX_ENTRIES:
                    raise ValueError("XDVDFS entry count resource limit exceeded")
                if is_directory:
                    enqueue_directory(path, file_sector, file_size, depth + 1)
                if right:
                    pending.append(right * 4)
                if left:
                    pending.append(left * 4)
        return result

    def read_file(self, path, max_bytes=MAX_XEX_BYTES):
        entry = self._by_path.get(path)
        if entry is None or entry.is_directory:
            raise ValueError(f"Disc file not found: {path}")
        if entry.size > max_bytes:
            raise ValueError("Disc file exceeds read limit; use streaming extraction")
        return self._read(self._extent(entry.sector, entry.size), entry.size)

    def inspect(self):
        data = self.read_file("default.xex")
        metadata = parse_xex(data)
        digest = hashlib.sha256(data).hexdigest()
        return {"format": "XDVDFS", "iso_size": self.size, "partition_offset": self.partition_offset,
                "file_count": sum(not entry.is_directory for entry in self.entries),
                "directory_count": sum(entry.is_directory for entry in self.entries),
                "entries": [asdict(entry) for entry in self.entries], "default_xex_sha256": digest,
                "supported_source": digest == EXPECTED_XEX_SHA256 and metadata.get("execution_info", {}).get("title_id") == EXPECTED_TITLE_ID,
                "xex": metadata}

    def _copy_file(self, entry, output):
        digest = hashlib.sha256()
        offset = self._extent(entry.sector, entry.size)
        remaining = entry.size
        with output.open("xb") as file:
            while remaining:
                chunk = self._read(offset, min(1024 * 1024, remaining))
                file.write(chunk)
                digest.update(chunk)
                offset += len(chunk)
                remaining -= len(chunk)
            file.flush()
            os.fsync(file.fileno())
        return digest.hexdigest()

    def extract(self, destination, paths=None, *, expected_sha256=EXPECTED_XEX_SHA256, expected_title_id=EXPECTED_TITLE_ID):
        """Verify default.xex, stage privately, then publish a new destination.

        Callers must exclusively own the output parent during extraction. Ancestor
        reparse points are refused; untrusted concurrent filesystem mutation is
        outside this local intake tool's threat model.
        """
        destination = Path(os.path.abspath(destination))
        _safe_ancestors(destination.parent)
        if os.path.lexists(destination):
            raise FileExistsError(f"Extraction destination already exists: {destination}")
        data = self.read_file("default.xex")
        digest = hashlib.sha256(data).hexdigest()
        if digest != expected_sha256:
            raise ValueError("default.xex SHA-256 does not match the supported source")
        metadata = parse_xex(data)
        if metadata.get("execution_info", {}).get("title_id") != expected_title_id:
            raise ValueError("default.xex Title ID does not match the supported source")
        if paths is None:
            selected = self.entries
        else:
            if not paths or len(set(paths)) != len(paths):
                raise ValueError("Extraction selection is empty or duplicated")
            selected = []
            for path in paths:
                entry = self._by_path.get(path)
                if entry is None or entry.is_directory:
                    raise ValueError(f"Selected disc file not found: {path}")
                selected.append(entry)
        if any(entry.path.split("/")[0].casefold() == MANIFEST_NAME.casefold() for entry in selected):
            raise ValueError("Disc path collides with extraction manifest")
        destination.parent.mkdir(parents=True, exist_ok=True)
        _safe_ancestors(destination.parent)
        stage = Path(tempfile.mkdtemp(prefix=f".{destination.name}.partial-", dir=destination.parent))
        try:
            manifest = {"schema_version": 1, "complete": False, "iso_size": self.size,
                        "partition_offset": self.partition_offset, "default_xex_sha256": digest,
                        "title_id": expected_title_id, "selection": "full_disc" if paths is None else list(paths), "files": []}
            for entry in selected:
                output = stage.joinpath(*entry.path.split("/"))
                _safe_ancestors(output.parent)
                if entry.is_directory:
                    output.mkdir(parents=True, exist_ok=True)
                    continue
                output.parent.mkdir(parents=True, exist_ok=True)
                file_digest = self._copy_file(entry, output)
                if entry.path == "default.xex" and file_digest != expected_sha256:
                    raise ValueError("default.xex changed during extraction")
                manifest["files"].append({"path": entry.path, "size": entry.size, "sha256": file_digest})
            manifest["complete"] = True
            with (stage / MANIFEST_NAME).open("x", encoding="utf-8", newline="\n") as file:
                json.dump(manifest, file, indent=2)
                file.write("\n")
                file.flush()
                os.fsync(file.fileno())
            _safe_ancestors(destination.parent)
            if os.path.lexists(destination):
                raise FileExistsError(f"Extraction destination appeared during extraction: {destination}")
            stage.rename(destination)
            return manifest
        except BaseException:
            # Only the unique staging directory created above is removed.
            shutil.rmtree(stage)
            raise
