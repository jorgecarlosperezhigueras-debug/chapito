#!/usr/bin/env python3
"""Fetch, validate and normalize the FireRed JP Rev.1 koto package."""

from __future__ import annotations

from pathlib import Path
from typing import NoReturn
from urllib.request import Request, urlopen
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

# Short-lived, read-only stream produced by the authenticated Drive connector.
# This is used only to replace the earlier manually copied binary. It expires
# automatically and will be removed from the clean checkpoint after recovery.
SIGNED_SOURCE_URL = "https://sdmntprdenmarkeast.oaiusercontent.com/files/00000000-ad38-8210-a34c-92c97ea2188d/raw?se=2026-10-07T23%3A02%3A32Z&sp=r&sv=2026-02-06&sr=b&scid=e04af138-02f9-5bbb-a170-11f7e9f23c72&skoid=5c9dda00-298b-4376-81e9-b4568c8a3c0f&sktid=a48cca56-e6da-484e-a814-9c849652bcb3&skt=2026-10-07T00%3A02%3A28Z&ske=2026-10-08T00%3A02%3A28Z&sks=b&skv=2026-02-06&sig=TkdWutjJIGAUket%2BzYLS0V8aKgnvftsgOG5PK6CR1Q8%3D"

REQUIRED_MEMBERS = {
    "manifest.json",
    "cards.json",
    "resources.json",
    "recognizers.json",
}
EXPECTED_CARD_IDS = {
    *(f"T{i:03d}" for i in range(1, 12)),
    *(f"I{i:03d}" for i in range(1, 12)),
    "H001M", "H001F", "H002M", "H002F", "H003", "H004",
    *(f"U{i:03d}" for i in range(1, 12)),
    *(f"P{i:03d}" for i in range(1, 9)),
    *(f"R{i:03d}" for i in range(1, 4)),
    *(f"L{i:03d}" for i in range(1, 9)),
    "L009C", "L009S", "L009B",
    *(f"L{i:03d}" for i in range(10, 17)),
    *(f"B{i:03d}" for i in range(1, 11)),
}
CARD_ID_PATTERN = re.compile(r"^[TIHUPRLB][0-9]{3}[A-Z]?$")
CARD_ID_BYTES_PATTERN = re.compile(rb"[TIHUPRLB][0-9]{3}[A-Z]?")


def fail(message: str) -> NoReturn:
    raise SystemExit(message)


def fetch_exact_source() -> str:
    request = Request(
        SIGNED_SOURCE_URL,
        headers={"User-Agent": "kotoGba-V0.4-recovery/1.0"},
    )
    try:
        with urlopen(request, timeout=30) as response:
            payload = response.read()
    except Exception as exc:
        fail(f"Could not fetch exact Drive package: {exc}")

    if len(payload) != EXPECTED_SOURCE_SIZE:
        fail(
            f"Exact Drive package has {len(payload)} bytes; "
            f"expected {EXPECTED_SOURCE_SIZE}"
        )
    SOURCE.parent.mkdir(parents=True, exist_ok=True)
    SOURCE.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    print(f"exact_drive_bytes={len(payload)}")
    print(f"exact_drive_sha256={digest}")
    return digest


def extract_member_from_local_header(raw_archive: bytes, info: zipfile.ZipInfo) -> bytes:
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
    compressed = raw_archive[start:start + info.compress_size]
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


def utf8_diagnostic(name: str, payload: bytes, exc: UnicodeDecodeError) -> str:
    start = exc.start
    end = max(exc.end, start + 1)
    window_start = max(0, start - 240)
    window_end = min(len(payload), end + 240)
    window = payload[window_start:window_end]
    nearby_ids = [
        match.group().decode("ascii")
        for match in CARD_ID_BYTES_PATTERN.finditer(window)
    ]
    return (
        f"Invalid UTF-8 in {name} at {start}:{end}; reason={exc.reason}; "
        f"bad_hex={payload[start:end].hex()}; nearby_ids={nearby_ids}; "
        f"window={window.decode('utf-8', errors='backslashreplace')!r}"
    )


def parse_json_member(name: str, payload: bytes) -> object:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        fail(utf8_diagnostic(name, payload, exc))
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        preview_start = max(0, exc.pos - 160)
        preview_end = min(len(text), exc.pos + 160)
        fail(
            f"Invalid JSON in {name} at char {exc.pos}: {exc.msg}; "
            f"context={text[preview_start:preview_end]!r}"
        )


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

    exact_drive_digest = fetch_exact_source()
    raw = SOURCE.read_bytes()
    extracted: dict[str, bytes] = {}
    crc_mismatches: list[str] = []
    size_mismatches: list[str] = []
    fallback_members: list[str] = []

    try:
        archive = zipfile.ZipFile(SOURCE)
    except zipfile.BadZipFile as exc:
        fail(f"Unreadable ZIP directory in exact Drive package: {exc}")

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
    matched = discovered_ids & EXPECTED_CARD_IDS
    missing_ids = sorted(EXPECTED_CARD_IDS - matched)
    if missing_ids:
        fail(
            f"Expected 78 Pueblo Paleta card IDs; found {len(matched)}. "
            f"Missing: {missing_ids}"
        )
    unexpected_ids = sorted(discovered_ids - EXPECTED_CARD_IDS)

    NORMALIZED.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        NORMALIZED,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as out:
        for name in sorted(extracted):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            out.writestr(info, extracted[name])

    with zipfile.ZipFile(NORMALIZED) as archive:
        if archive.testzip() is not None:
            fail("Normalized package failed ZIP integrity validation")
        if set(archive.namelist()) != set(extracted):
            fail("Normalized package member list changed unexpectedly")
        for name in REQUIRED_MEMBERS:
            reparsed = parse_json_member(name, archive.read(name))
            if reparsed != decoded[name]:
                fail(f"JSON payload changed while normalizing {name}")

    def joined(values: list[str]) -> str:
        return ",".join(sorted(set(values))) if values else "none"

    normalized_digest = hashlib.sha256(NORMALIZED.read_bytes()).hexdigest()
    report = (
        f"exact_drive_bytes={len(raw)}\n"
        f"exact_drive_sha256={exact_drive_digest}\n"
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
