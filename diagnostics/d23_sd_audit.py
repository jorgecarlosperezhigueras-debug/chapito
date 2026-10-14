#!/usr/bin/env python3
"""Read-only D23 data audit. This is NOT a ROM or an in-game validation.

Compare KANJI.K20 and BPRJ_01.ks5 in a FAT image with a reference folder/ZIP.
No assumed SD paths: --expected-path must come from the actual D23 source.
Close the emulator first. Partition offsets are explicit, in bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import stat
import sys
import zipfile
from pathlib import Path, PurePosixPath
from typing import BinaryIO

TARGETS = ("KANJI.K20", "BPRJ_01.ks5")
MAX_FILE_BYTES = 1024 * 1024 * 1024
MAX_ENTRIES = 100000


def canonical(path: str) -> str:
    """Normalize a FAT path for comparison; do not resolve parent traversal."""
    path = path.replace("\\", "/")
    if "\x00" in path or ".." in PurePosixPath(path).parts:
        raise ValueError("Unsafe path")
    return "/" + str(PurePosixPath(path)).lstrip("/").casefold()


def fingerprint(stream: BinaryIO, declared_size: int) -> dict:
    """Read the WHOLE logical file, with bounded memory and size checks."""
    if not 0 <= declared_size <= MAX_FILE_BYTES:
        raise ValueError("File size outside audit limit")
    sha = hashlib.sha256()
    header = b""
    total = 0
    while True:
        chunk = stream.read(min(65536, declared_size + 1 - total))
        if not chunk:
            break
        if len(header) < 64:
            header += chunk[:64 - len(header)]
        total += len(chunk)
        if total > declared_size:
            raise OSError("Read exceeded declared size")
        sha.update(chunk)
    if total != declared_size:
        raise OSError(f"Incomplete read: {total}/{declared_size} bytes")
    return {"size": total, "sha256": sha.hexdigest(), "header_hex": header.hex()}


def reference_inventory(reference: Path) -> dict:
    """Read matching files in place. ZIP entries are never extracted."""
    found = {name: [] for name in TARGETS}
    names = {name.casefold(): name for name in TARGETS}
    def record(name: str, stream: BinaryIO, size: int) -> None:
        key = names[PurePosixPath(name).name.casefold()]
        found[key].append({"path": name, **fingerprint(stream, size)})

    if reference.is_dir():
        for index, path in enumerate(reference.rglob("*")):
            if index >= MAX_ENTRIES:
                raise ValueError("Reference entry limit exceeded")
            if path.name.casefold() not in names:
                continue
            if path.is_symlink():
                raise ValueError("Reference symlinks are not accepted")
            if path.is_file():
                with path.open("rb") as stream:
                    record(path.relative_to(reference).as_posix(), stream, path.stat().st_size)
    else:
        with zipfile.ZipFile(reference) as archive:
            if len(archive.infolist()) > MAX_ENTRIES:
                raise ValueError("ZIP entry limit exceeded")
            for info in archive.infolist():
                name = info.filename.replace("\\", "/")
                if info.is_dir() or name.startswith("__MACOSX/"):
                    continue
                if PurePosixPath(name).name.casefold() not in names:
                    continue
                canonical(name)
                if stat.S_ISLNK(info.external_attr >> 16):
                    raise ValueError("ZIP symlinks are not accepted")
                with archive.open(info) as stream:
                    record(name, stream, info.file_size)
    return found


def inspect_filesystem(store, refs: dict, expected_paths: dict | None = None) -> dict:
    """The store must be read-only. Also used with FAT fixtures in CI."""
    expected_paths = expected_paths or {}
    if set(expected_paths) - set(TARGETS):
        raise ValueError("Unknown target in expected paths")
    expected_paths = {k: canonical(v) for k, v in expected_paths.items()}
    candidates = {name: [] for name in TARGETS}
    names = {name.casefold(): name for name in TARGETS}
    for index, path in enumerate(store.walk.files()):
        if index >= MAX_ENTRIES:
            raise ValueError("Image entry limit exceeded")
        key = names.get(PurePosixPath(path).name.casefold())
        if key:
            candidates[key].append(path)

    rows = []
    for name in TARGETS:
        row = {"name": name, "status": "NOT_CHECKED", "locations": candidates[name],
               "expected_path": expected_paths.get(name), "reference": refs[name]}
        unique_refs = {(r["size"], r["sha256"]) for r in refs[name]}
        if not unique_refs:
            row["status"] = "REFERENCE_MISSING"
        elif len(unique_refs) != 1:
            row["status"] = "REFERENCE_AMBIGUOUS"
        elif next(iter(unique_refs))[0] == 0:
            row["status"] = "REFERENCE_EMPTY"
        elif not candidates[name]:
            row["status"] = "FILE_MISSING"
        else:
            locations = candidates[name]
            if name in expected_paths:
                locations = [p for p in locations if canonical(p) == expected_paths[name]]
                if not locations:
                    row["status"] = "PATH_MISMATCH"
            if row["status"] == "NOT_CHECKED":
                if len(locations) != 1:
                    row["status"] = "MULTIPLE_LOCATIONS"
                else:
                    row["selected_path"] = locations[0]
                    try:
                        with store.openbin(locations[0], "r") as stream:
                            actual = fingerprint(stream, store.getsize(locations[0]))
                        row["actual"] = actual
                        ref = refs[name][0]
                        row["header_matches"] = actual["header_hex"] == ref["header_hex"]
                        if actual["size"] != ref["size"]:
                            row["status"] = "SIZE_MISMATCH"
                        elif actual["sha256"] != ref["sha256"]:
                            row["status"] = "BYTE_MISMATCH"
                        else:
                            row["status"] = "BYTE_IDENTICAL"
                    except Exception as exc:
                        row["status"] = "READ_ERROR"
                        row["error"] = f"{type(exc).__name__}: {exc}"
        rows.append(row)

    content_ok = all(row["status"] == "BYTE_IDENTICAL" for row in rows)
    all_paths_given = set(expected_paths) == set(TARGETS)
    return {
        "schema": "kotogba-d23-sd-audit-1",
        "result": ("PASS_CONTENT_AND_PATHS" if all_paths_given else "PASS_CONTENT_ONLY")
                  if content_ok else "FAIL",
        "path_validation": "COMPLETE" if all_paths_given else "INCOMPLETE",
        "read_only": True,
        "in_game_reads_tested": False,
        "vocabulary_to_kanji_tested": False,
        "notice": "Matching bytes do not validate package semantics, DLDI, runtime memory, or navigation.",
        "files": rows,
    }


def audit(image: Path, reference: Path, offset: int = 0,
          expected_paths: dict | None = None) -> dict:
    """Return a machine-readable result, including access/format failures."""
    report = {"schema": "kotogba-d23-sd-audit-1", "result": "ERROR", "read_only": True,
              "in_game_reads_tested": False, "vocabulary_to_kanji_tested": False}
    try:
        if offset < 0:
            raise ValueError("Negative partition offset")
        if not image.is_file():
            raise ValueError("Image must be a regular file, not a physical device")
        report["stage"] = "reference"
        refs = reference_inventory(reference)
        report["stage"] = "fat_image"
        from pyfatfs.PyFatFS import PyFatFS
        with PyFatFS(str(image), read_only=True, offset=offset) as store:
            result = inspect_filesystem(store, refs, expected_paths)
        return {**result, "image": str(image), "reference": str(reference), "offset": offset}
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--offset", type=int, default=0, help="FAT volume offset, in bytes")
    parser.add_argument("--expected-path", action="append", default=[], metavar="NAME=/path")
    parser.add_argument("--report", type=Path, help="Create a NEW JSON file; never overwrite")
    args = parser.parse_args()
    expected = {}
    for item in args.expected_path:
        name, separator, path = item.partition("=")
        if not separator or name not in TARGETS or not path or name in expected:
            parser.error("Expected NAME=/path, once per target")
        expected[name] = path
    result = audit(args.image, args.reference, args.offset, expected)
    output = json.dumps(result, indent=2, ensure_ascii=False)
    if args.report:
        with args.report.open("x", encoding="utf-8") as stream:
            stream.write(output + "\n")
    print(output)
    return 0 if result["result"].startswith("PASS_") else 1


if __name__ == "__main__":
    sys.exit(main())
