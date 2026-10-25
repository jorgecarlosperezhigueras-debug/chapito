#!/usr/bin/env python3
"""Read-only checkpoint verification and immutable LOCAL batch provenance.

Standard library only. No network, extraction, memory import, or git push.
A prepared receipt is not an imported review and is useless without its payload.
"""
from __future__ import annotations
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
HEX = re.compile(r"[0-9a-f]{64}\Z")
IDENT = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}\Z")
KEYS = {"schema", "batch_id", "base_sha256", "input_sha256", "output_sha256", "occurrence_ids", "status"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_receipt(value: object) -> dict:
    require(isinstance(value, dict), "El recibo debe ser un objeto JSON")
    require(set(value) == KEYS, "Campos ausentes o no permitidos: no incluir contenido privado")
    require(value["schema"] == "kotogba.prepared-batch/1", "Esquema de recibo desconocido")
    require(isinstance(value["batch_id"], str) and bool(IDENT.fullmatch(value["batch_id"])), "ID de lote inválido")
    for field in ("base_sha256", "input_sha256", "output_sha256"):
        require(isinstance(value[field], str) and bool(HEX.fullmatch(value[field])), "Huella inválida: " + field)
    require(value["status"] == "prepared", "Este diario NO acredita importación")
    ids = value["occurrence_ids"]
    require(isinstance(ids, list) and 1 <= len(ids) <= 100, "Cada lote debe tener entre 1 y 100 apariciones")
    require(all(isinstance(x, str) and IDENT.fullmatch(x) for x in ids), "ID de aparición inválido")
    require(len(ids) == len(set(ids)), "Apariciones duplicadas dentro del lote")
    return dict(value, occurrence_ids=sorted(ids))


def record(journal: Path, packet: object) -> str:
    value = validate_receipt(packet)
    require(not journal.is_symlink(), "El diario no puede ser un enlace simbólico")
    journal.mkdir(parents=True, exist_ok=True)
    lock_flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(journal / ".writer.lock", lock_flags, 0o600)
    with os.fdopen(fd, "a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        target = journal / (value["batch_id"] + ".json")
        body = canonical(value)
        require(not target.is_symlink(), "El destino no puede ser un enlace simbólico")
        if target.exists():
            require(target.read_bytes() == body, "Conflicto: el lote ya existe con otros bytes")
            return "already_present"
        ids = set(value["occurrence_ids"])
        for old_path in sorted(journal.glob("*.json")):
            require(not old_path.is_symlink(), "Recibo enlazado no permitido")
            old = validate_receipt(json.loads(old_path.read_text("utf-8")))
            require(old_path.stem == old["batch_id"], "Nombre de recibo incoherente")
            if old["base_sha256"] == value["base_sha256"]:
                require(not ids.intersection(old["occurrence_ids"]), "Solapamiento con lote " + old["batch_id"])
        temp_fd, temp_name = tempfile.mkstemp(prefix=".pending-", dir=journal)
        try:
            with os.fdopen(temp_fd, "wb") as stream:
                stream.write(body)
                stream.flush()
                os.fsync(stream.fileno())
            # Hard-link publishes exclusively: it cannot overwrite an existing receipt.
            os.link(temp_name, target)
            directory_fd = os.open(journal, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temp_name)
        return "created"


def verify_archive(path: Path, expected: dict) -> dict:
    require(path.is_file() and not path.is_symlink(), "No hay un archivo regular para verificar")
    require(path.stat().st_size == expected["bytes"], "Tamaño de archivo incorrecto")
    actual = digest(path)
    require(actual == expected["sha256"], "SHA-256 de archivo incorrecto")
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        require(len(infos) <= 100000, "Demasiados miembros ZIP")
        require(sum(i.file_size for i in infos) <= 32 * 1024**3, "ZIP expandido excede el límite de lectura")
        names: set[str] = set()
        for info in infos:
            raw = info.filename
            p = PurePosixPath(raw)
            require(bool(raw) and not p.is_absolute() and ".." not in p.parts and "\\" not in raw and ":" not in raw and "\x00" not in raw, "Ruta ZIP insegura")
            key = str(p).casefold()
            require(key not in names, "Nombre ZIP duplicado")
            names.add(key)
            require(not stat.S_ISLNK(info.external_attr >> 16), "Enlace simbólico ZIP no permitido")
            require(not (info.flag_bits & 1), "ZIP cifrado no verificable")
        bad = archive.testzip()
        require(bad is None, "CRC ZIP incorrecto")
    return {"scope": "archive_integrity_only", "sha256": actual, "bytes": path.stat().st_size, "members": len(infos), "passed": True, "extracted": False}


def validate_state(state: dict) -> None:
    require(state.get("schema") == "kotogba.recovery-state/1", "Estado de proyecto desconocido")
    require(state.get("project") == "kotoGBA", "Proyecto incorrecto")
    for key in ("current_sources_imported", "current_build_verified", "runtime_tests_verified", "physical_console_verified", "installable"):
        require(type(state.get(key)) is bool, "El estado requiere booleano: " + key)
    if state["installable"]:
        require(all(state[k] for k in ("current_sources_imported", "current_build_verified", "runtime_tests_verified")), "Una entrega instalable necesita fuentes, compilación y pruebas actuales")


def release_gate(state: dict) -> None:
    validate_state(state)
    require(state["installable"], "BLOQUEADO: esta infraestructura no es una entrega instalable")


def check_tracked(root: Path) -> None:
    raw = subprocess.check_output(["git", "ls-files", "-z"], cwd=root)
    forbidden = {".gba", ".nds", ".bios", ".sav", ".srm", ".sqlite", ".db", ".pdf", ".rar", ".zip", ".ttf", ".otf", ".woff", ".woff2", ".pem", ".key"}
    for item in raw.decode("utf-8").split("\0"):
        if not item:
            continue
        p = PurePosixPath(item)
        require(p.suffix.lower() not in forbidden and not re.search(r"\.zip\.\d+$|\.sqlite(?:-|\.)", item, re.I), "Archivo restringido en Git: " + item)
        require(not any(x.lower() in {"private", ".local"} for x in p.parts), "Directorio privado en Git")
        require(p.name != ".env" and not p.name.startswith(".env."), "Archivo de entorno en Git")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("contract")
    sub.add_parser("release-gate")
    p = sub.add_parser("record")
    p.add_argument("--packet", type=Path, required=True)
    p.add_argument("--journal", type=Path, required=True)
    p = sub.add_parser("verify-archive")
    p.add_argument("--key", choices=("b11", "b19_full", "b19_sd"), required=True)
    p.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "record":
            print(record(args.journal, json.loads(args.packet.read_text("utf-8"))))
        elif args.command == "verify-archive":
            locks = json.loads((ROOT / "project/CHECKPOINTS.json").read_text("utf-8"))
            print(json.dumps(verify_archive(args.archive, locks["archives"][args.key]), sort_keys=True))
        else:
            state = json.loads((ROOT / "project/STATE.json").read_text("utf-8"))
            validate_state(state)
            if args.command == "release-gate":
                release_gate(state)
            else:
                check_tracked(ROOT)
                locks = json.loads((ROOT / "project/CHECKPOINTS.json").read_text("utf-8"))
                require(locks.get("schema") == "kotogba.checkpoint-lock/1", "Lock desconocido")
                for value in locks["archives"].values():
                    require(bool(HEX.fullmatch(value["sha256"])) and type(value["bytes"]) is int and value["bytes"] > 0, "Lock inválido")
                print(json.dumps({"scope": "infrastructure_contract_only", "passed": True, "current_sources_imported": state["current_sources_imported"], "installable": state["installable"]}))
        return 0
    except (ValueError, OSError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as exc:
        print("ERROR:", exc)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
