#!/usr/bin/env python3
"""Reproduce the frozen B19 executable without publishing its private inputs.

Standard-library only. No downloads, database mutations, or uploads are made.
The compiler and every input are checked before code from the bundle runs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time
import zipfile


REPO = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def safe_name(name: str) -> PurePosixPath:
    require(isinstance(name, str) and bool(name), "Empty path")
    path = PurePosixPath(name)
    require(not path.is_absolute() and "\\" not in name and
            all(part not in ("", ".", "..") for part in name.split("/")) and
            ":" not in name and "\x00" not in name, "Unsafe path: " + name)
    return path


def digest(path: Path) -> dict:
    with path.open("rb") as stream:
        checksum = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"bytes": path.stat().st_size, "sha256": checksum}


def check_info(actual: dict, expected: dict, name: str) -> None:
    require(all(actual[key] == expected[key] for key in ("bytes", "sha256")),
            "Input does not match the B19 lock: " + name)


def contained_file(root: Path, relative: str) -> Path:
    safe_name(relative)
    path = root / relative
    require(path.is_file() and not path.is_symlink(), "Missing or linked input: " + relative)
    require(path.resolve().is_relative_to(root.resolve()), "Input escapes its root: " + relative)
    return path


def load_locks(repo: Path) -> tuple[dict, dict]:
    current = repo / "current"
    sources = json.loads(contained_file(current, "B19_SOURCES.json").read_text())
    inputs = json.loads(contained_file(current, "PRIVATE_BUILD_INPUTS.json").read_text())
    require(sources.get("schema") == "kotogba.b19.sources/1", "Unknown source lock")
    require(inputs.get("schema") == "kotogba.b19.private-build-inputs/1", "Unknown input lock")
    require(len(sources["source_files"]) == inputs["source_files"], "Source count differs")
    return sources, inputs


def verify_sources(repo: Path, sources: dict) -> dict:
    counts = {"public": 0, "private": 0}
    for name, expected in sources["source_files"].items():
        safe_name(name)
        storage = expected["storage"]
        require(storage in counts, "Unknown source storage: " + name)
        counts[storage] += 1
        if storage == "public":
            path = contained_file(repo / "current", expected["path"])
            check_info(digest(path), expected, name)
        else:
            safe_name(expected["member"])
    require(counts["public"] == sources["public_source_files"] and
            counts["private"] == sources["private_source_files"], "Source partitions differ")
    return counts


def verify_bundle(archive: Path, inputs: dict) -> None:
    check_info(digest(archive), inputs["archive"], archive.name)
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        require(len(names) == len(set(names)), "Duplicate archive member")
        require(set(names) == set(inputs["entries"]), "Archive member set differs from lock")
        for entry in bundle.infolist():
            safe_name(entry.filename)
            require(not entry.is_dir() and not stat.S_ISLNK(entry.external_attr >> 16),
                    "Non-file archive member: " + entry.filename)
            expected = inputs["entries"][entry.filename]
            require(entry.file_size == expected["bytes"], "Archive member size differs")
            with bundle.open(entry) as stream:
                actual = {"bytes": entry.file_size,
                          "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
            check_info(actual, expected, entry.filename)


def checkpoint_input(checkpoint_root: Path, entry: dict) -> Path:
    """Resolve one explicitly locked path; never search the checkpoint tree."""
    relative = str(safe_name(entry["source_path"]))
    role = entry["source_role"]
    if role == "frozen_source":
        relative = "desarrollo/pruebas_HITO_B/build/candidate19/sources/" + relative
    else:
        require(role in {"project_prerequisite", "retained_sdk"},
                "Unknown checkpoint input role")
    return contained_file(checkpoint_root, relative)


def make_inputs(repo: Path, checkpoint_root: Path, output: Path) -> dict:
    """Recreate the exact private bundle from a restored B19/B10 checkpoint."""
    sources, inputs = load_locks(repo)
    verify_sources(repo, sources)
    require(checkpoint_root.is_dir(), "Checkpoint root is not a directory")
    require(not output.exists() and not output.is_symlink(),
            "Output already exists; choose a new filename")
    resolved = {}
    # Verify the full selection before creating any output or temporary file.
    for name, expected in inputs["entries"].items():
        safe_name(name)
        source = checkpoint_input(checkpoint_root, expected)
        check_info(digest(source), expected, name)
        resolved[name] = source
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=".b19-inputs-", suffix=".zip.tmp",
                                          dir=output.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "w+b") as target:
            with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED,
                                 compresslevel=6, allowZip64=True) as bundle:
                for name in sorted(resolved):
                    # Check the actual bytes written as well: an input changed
                    # after the initial preflight must never silently enter it.
                    data = resolved[name].read_bytes()
                    actual = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                    check_info(actual, inputs["entries"][name], name)
                    info = zipfile.ZipInfo(name, date_time=(2026, 10, 25, 0, 0, 0))
                    info.create_system = 3
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info.external_attr = 0o100644 << 16
                    bundle.writestr(info, data, compresslevel=6)
            target.flush()
            os.fsync(target.fileno())
        actual = digest(temporary)
        check_info(actual, inputs["archive"], "rebuilt private bundle")
        # A hard link publishes the validated complete file atomically and
        # refuses to overwrite a destination created by a competing writer.
        os.link(temporary, output)
        directory_fd = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        temporary.unlink(missing_ok=True)
    return {"schema": "kotogba.b19.private-inputs-reconstruction/1", "passed": True,
            "entries_verified": len(resolved), "archive": actual,
            "matches_locked_bundle": True, "source_mutations": 0}


def extract_sdk(archive: Path, destination: Path, limit: int) -> None:
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        total = 0
        for entry in members:
            safe_name(entry.name.rstrip("/"))
            require(entry.isfile() or entry.isdir() or entry.issym() or entry.islnk(),
                    "Unsupported SDK archive member")
            total += entry.size if entry.isfile() else 0
        require(total <= limit, "SDK exceeds the locked expansion limit")
        # Python's data filter checks symlink/hardlink targets and containment.
        bundle.extractall(destination, members=members, filter="data")


def stage(repo: Path, archive: Path, sources: dict, inputs: dict, temporary: Path) -> Path:
    private = temporary / "inputs"
    with zipfile.ZipFile(archive) as bundle:
        for name, expected in inputs["entries"].items():
            target = private / safe_name(name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(name) as src, target.open("xb") as dst:
                shutil.copyfileobj(src, dst)
            check_info(digest(target), expected, name)
    project = temporary / "project"
    for name, expected in sources["source_files"].items():
        source = (contained_file(repo / "current", expected["path"])
                  if expected["storage"] == "public"
                  else contained_file(private, expected["member"]))
        target = project / safe_name(name)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        check_info(digest(target), expected, name)
    for name, expected in inputs["entries"].items():
        if expected["source_role"] != "project_prerequisite":
            continue
        target = project / safe_name(expected["source_path"])
        require(not target.exists(), "Private input overlaps an existing source")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(contained_file(private, name), target)
    sdk_archive = private / "sdk/sdk_linux_R3_conservado.tar.gz"
    extract_sdk(sdk_archive, temporary / "sdk", inputs["sdk_unpacked_limit_bytes"])
    return project


def build(repo: Path, archive: Path, output: Path, jobs: int) -> dict:
    require(1 <= jobs <= 16, "Jobs must be from 1 to 16")
    sources, inputs = load_locks(repo)
    counts = verify_sources(repo, sources)
    verify_bundle(archive, inputs)
    # Never reuse a half-built tree or replace a previous evidence directory.
    require(not output.exists(), "Output already exists; choose a new directory")
    output.mkdir(parents=True)
    report = {"schema": "kotogba.b19.build-verification/1", "candidate": "B19",
              "source_files": counts, "private_bundle": inputs["archive"],
              "inputs_verified_before_execution": True, "passed": False,
              "emulator_tested": False, "physical_console_tested": False,
              "sd_data_updated": False}
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="b19-work-", dir=output) as tmp:
            temporary = Path(tmp)
            project = stage(repo, archive, sources, inputs, temporary)
            tempdir = temporary / "tmp"
            tempdir.mkdir()
            env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
                   "KOTO_SDK_ROOT": str(temporary / "sdk"), "KOTO_BUILD_JOBS": str(jobs),
                   "TMPDIR": str(tempdir), "PYTHONDONTWRITEBYTECODE": "1"}
            with (output / "build.log").open("w") as log:
                proc = subprocess.run(["/usr/bin/bash", "desarrollo/compilar_HITO_B.sh"],
                                      cwd=project, env=env, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=600, check=False)
            report["build_returncode"] = proc.returncode
            require(proc.returncode == 0, "B19 compilation failed; see build.log")
            outputs = {}
            for name, expected in inputs["expected_outputs"].items():
                source = (project / name if name.endswith(".nds") else
                          project / "desarrollo/fuentes/code/core/arm9" / name)
                actual = digest(source)
                check_info(actual, expected, name)
                shutil.copyfile(source, output / name)
                outputs[name] = {**actual, "matches_frozen_B19": True}
            for name, relative in {
                "arm9.elf": "desarrollo/fuentes/code/core/arm9/arm9.elf",
                "arm7.elf": "desarrollo/fuentes/code/core/arm7/GBARunner3.elf",
            }.items():
                shutil.copyfile(project / relative, output / name)
                outputs[name] = {**digest(output / name), "debug_paths_may_differ": True}
            report.update(outputs=outputs, passed=True)
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        report["seconds"] = round(time.monotonic() - started, 3)
        (output / "BUILD_VERIFICATION.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["check-sources", "verify-inputs", "make-inputs", "build"])
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--inputs", type=Path)
    parser.add_argument("--checkpoint-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    repo = args.repo.resolve()
    sources, inputs = load_locks(repo)
    result = {"source_files": verify_sources(repo, sources), "passed": True}
    if args.command == "make-inputs":
        if args.checkpoint_root is None or args.output is None:
            parser.error("make-inputs requires --checkpoint-root and --output")
        result = make_inputs(repo, args.checkpoint_root.resolve(strict=True),
                             args.output.absolute())
    elif args.command != "check-sources":
        if args.inputs is None:
            parser.error("--inputs is required")
        archive = args.inputs.resolve(strict=True)
        if args.command == "verify-inputs":
            verify_bundle(archive, inputs)
            result["private_inputs_verified"] = True
        else:
            if args.output is None:
                parser.error("--output is required")
            result = build(repo, archive, args.output.resolve(), args.jobs)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
