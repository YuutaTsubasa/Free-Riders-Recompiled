#!/usr/bin/env python3
"""Inspect a local XDVDFS image, extract verified assets, or read XEX metadata."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile

from freeriders.disc import DiscImage, _safe_ancestors
from freeriders.xex import MAX_XEX_BYTES, parse_xex


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("inspect", "extract"):
        child = commands.add_parser(command)
        child.add_argument("--iso", type=Path, required=True)
        child.add_argument("--output", type=Path, required=True)
        if command == "extract":
            child.add_argument("--path", action="append", help="Exact file path; repeat for multiple files. Omit to extract the full disc.")
    child = commands.add_parser("xex")
    child.add_argument("path", type=Path)
    child.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        source = args.path if args.command == "xex" else args.iso
        if args.output and (args.output.resolve() == source.resolve()
                            or (args.output.exists() and source.exists() and args.output.samefile(source))):
            raise ValueError("Output must not overwrite the source file")
        if args.command == "xex":
            with args.path.open("rb") as file:
                data = file.read(MAX_XEX_BYTES + 1)
            report = parse_xex(data)
            report["sha256"] = hashlib.sha256(data).hexdigest()
        else:
            with DiscImage(args.iso) as image:
                if args.command == "extract":
                    report = image.extract(args.output, paths=args.path)
                    print(json.dumps({"complete": report["complete"], "files_extracted": len(report["files"]),
                                      "destination": str(args.output)}, indent=2))
                    return 0
                report = image.inspect()
        text = json.dumps(report, indent=2) + "\n"
        if args.output:
            _write_report(args.output, text)
            print(f"Wrote {args.output}")
        else:
            print(text, end="")
        return 0
    except (OSError, ValueError) as error:
        print(f"Source intake failed: {error}", file=sys.stderr)
        return 1


def _write_report(output, text):
    output = Path(os.path.abspath(output))
    _safe_ancestors(output.parent)
    if os.path.lexists(output):
        info = output.lstat()
        if not stat.S_ISREG(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("Report output must be a regular file, without a reparse point")
    output.parent.mkdir(parents=True, exist_ok=True)
    _safe_ancestors(output.parent)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", prefix=".report-", dir=output.parent, delete=False) as file:
            temporary = Path(file.name)
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
        _safe_ancestors(output.parent)
        temporary.replace(output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


if __name__ == "__main__":
    raise SystemExit(main())
