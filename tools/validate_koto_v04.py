#!/usr/bin/env python3
"""Validate the FireRed JP Rev.1 koto package and normalize ZIP metadata.

The source prototype contains valid compressed JSON payloads but may carry stale
CRC/uncompressed-size fields in the ZIP directory. This script never edits the
JSON payloads: it extracts the actual DEFLATE streams, validates their semantic
contents, and writes a deterministic ZIP with corrected metadata.
"""

from __future__ import annotations

from pathlib import Path
import hashlib
import json
import re
import struct
import zipfile
import zlib

SOURCE = Path("packages/BPRJ_01.koto")
NORMALIZED = Path("validated_packages/BPRJ_01.koto")
REPORT = Path("PACKAGE_VALIDATION_V04.txt")
EXPECTED_SOURCE_SIZE = 22_364
REQUIRED_MEMBERS = {
    "manifest.json",
    "cards.json",
    "resources.json",
    "recognizers.json",
}

EXPECTED_CARD_IDS = {
    *(f"T{i:03d}" for i in range(1, 12)),
    *(f"I{i:03d}" for i in range(1, 12)),
    "H001M",
    "H001F",
    "H002M",
    "H002F",
    "H003",
    "H004",
    *(f"U{i:03d}" for i in range(1, 12)),
    *(f"P{i:03d}" for i in range(1, 9)),
    *(f"R{i:03d}" for i in range(1, 4)),
    *(f"L{i:03d}" for i in range(1, 9)),
    "L009C",
    "L009S",
    "L009B",
    *(f"L{i:03d}" for i in range(10, 17)),
    *(f"B{i:03d}" for i in range(1, 11)),
}
CARD_ID_PATTERN = re.compile(r"^[TIHUPRLB][0-9]{3}[A-Z]?$")


def fail(message: str) -> "NoReturn":  # type: ignore[name-defined]
    raise SystemExit(message)


def extract_member_from_local_header(
    raw_archive: bytes,
    info: zipfile.ZipInfo,
) -> bytes:
    """Read and decompress one member without trusting central CRC/size fields."""

    if info.header_offset < 0 or info.header_offset + 30 > len(raw_archive):
        fail(f"Invalid local-header offset for {info.filename}")

    (
        signature,
        _version,
        _flags,
        method,
        _time,
        _date,
        _crc,
        _compressed_size,
        _uncompressed_size,
        name_length,
        extra_length,
    ) = struct.unpack_from("<IHHHHHIIIHH", raw_archive, info.header_offset)

    if signature != 0x04034B50:
        fail(f"Bad local-header signature for {info.filename}")

    start = info.header_offset + 30 + name_length + extra_length
    end = start + info.compress_size
    compressed = raw_archive[start:end]
    if len(compressed) != info.compress_size:
        fail(f"Truncated compressed data for {info.filename}")

    try:
        if method == zipfile.ZIP_STORED:
            return compressed
        if method == zipfile.ZIP_DEFLATED:
            return zlib.decompress(compressed, -15)
    except zlib.error as exc:
        fail(f"Invalid DEFLATE stream for {info.filename}: {exc}")

    fail(f"Unsupported compression method {method} for {info.filename}")


def parse_json_member(name: str, payload: bytes) -> object:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        fail(f"Invalid UTF-8 in {name}: {exc}")

    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        preview_start = max(0, exc.pos - 80)
        preview_end = min(len(text), exc.pos + 80)
        preview = repr(text[preview_start:preview_end])
        fail(f"Invalid JSON in {name} at char {exc.pos}: {exc.msg}; context={preview}")


def collect_card_ids(value: object, output: set[str]) -> None:
    if isinstance(value, str):
        if CARD_ID_PATTERN.fullmatch(value):
            output.add(value)
        return
    if isinstance(value, dict):
        for key, child in value.items():
            if isinstance(key, str) and CARD_ID_PATTERN.fullmatch(key):
                output.add(key)
            collect_card_ids(child, output)
        return
    if isinstance(value, list):
        for child in value:
            collect_card_ids(child, output)


def main() -> None:
    if len(EXPECTED_CARD_IDS) != 78:
        fail(f"Internal expected-card set is {len(EXPECTED_CARD_IDS)}, not 78")
    if not SOURCE.is_file():
        fail(f"Missing {SOURCE}")
    if SOURCE.stat().st_size != EXPECTED_SOURCE_SIZE:
        fail(
            f"Unexpected source package size: {SOURCE.stat().st_size} "
            f"(expected {EXPECTED_SOURCE_SIZE})"
        )

    raw = SOURCE.read_bytes()
    extracted: dict[str, bytes] = {}
    crc_mismatches: list[str] = []
    size_mismatches: list[str] = []
    fallback_members: list[str] = []

    try:
        archive = zipfile.ZipFile(SOURCE)
    except zipfile.BadZipFile as exc:
        fail(f"Unreadable ZIP directory in {SOURCE}: {exc}")

    with archive:
        infos = archive.infolist()
        names = {info.filename for info in infos}
        missing = REQUIRED_MEMBERS - names
        if missing:
            fail(f"Missing package members: {sorted(missing)}")
        if len(names) != len(infos):
            fail("Duplicate member names found in source package")

        for info in infos:
            try:
                payload = archive.read(info)
            except (zipfile.BadZipFile, RuntimeError):
                payload = extract_member_from_local_header(raw, info)
                fallback_members.append(info.filename)

            actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
            if actual_crc != info.CRC:
                crc_mismatches.append(
                    f"{info.filename}:{actual_crc:08x}!={info.CRC:08x}"
                )
            if len(payload) != info.file_size:
                size_mismatches.append(
                    f"{info.filename}:{len(payload)}!={info.file_size}"
                )
            extracted[info.filename] = payload

    decoded = {
        name: parse_json_member(name, extracted[name]) for name in REQUIRED_MEMBERS
    }

    discovered_ids: set[str] = set()
    collect_card_ids(decoded["cards.json"], discovered_ids)
    present_expected_ids = discovered_ids & EXPECTED_CARD_IDS
    missing_ids = sorted(EXPECTED_CARD_IDS - present_expected_ids)
    if missing_ids:
        fail(
            f"Expected 78 Pueblo Paleta card IDs; found {len(present_expected_ids)}. "
            f"Missing: {missing_ids}"
        )

    unexpected_ids = sorted(discovered_ids - EXPECTED_CARD_IDS)

    NORMALIZED.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        NORMALIZED,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as normalized_archive:
        for name in sorted(extracted):
            normalized_info = zipfile.ZipInfo(
                name,
                date_time=(1980, 1, 1, 0, 0, 0),
            )
            normalized_info.compress_type = zipfile.ZIP_DEFLATED
            normalized_info.external_attr = 0o100644 << 16
            normalized_archive.writestr(normalized_info, extracted[name])

    with zipfile.ZipFile(NORMALIZED) as normalized_archive:
        bad_member = normalized_archive.testzip()
        if bad_member is not None:
            fail(f"Normalized package is corrupt at {bad_member}")
        normalized_names = set(normalized_archive.namelist())
        if normalized_names != set(extracted):
            fail("Normalized package member list changed unexpectedly")
        for name in REQUIRED_MEMBERS:
            reparsed = parse_json_member(name, normalized_archive.read(name))
            if reparsed != decoded[name]:
                fail(f"JSON payload changed while normalizing {name}")

    def joined(values: list[str]) -> str:
        return ",".join(sorted(set(values))) if values else "none"

    source_digest = hashlib.sha256(raw).hexdigest()
    normalized_digest = hashlib.sha256(NORMALIZED.read_bytes()).hexdigest()
    report = (
        f"source_package={SOURCE}\n"
        f"source_bytes={SOURCE.stat().st_size}\n"
        f"source_sha256={source_digest}\n"
        f"normalized_package={NORMALIZED}\n"
        f"normalized_bytes={NORMALIZED.stat().st_size}\n"
        f"normalized_sha256={normalized_digest}\n"
        f"fallback_members={joined(fallback_members)}\n"
        f"source_crc_mismatches={joined(crc_mismatches)}\n"
        f"source_size_mismatches={joined(size_mismatches)}\n"
        f"unexpected_card_ids={','.join(unexpected_ids) if unexpected_ids else 'none'}\n"
        "cards=78\n"
        "game_code=BPRJ\n"
        "revision=01\n"
        "json_payloads_preserved=yes\n"
        "status=PASS\n"
    )
    REPORT.write_text(report, encoding="utf-8")
    print(report, end="")


if __name__ == "__main__":
    main()
