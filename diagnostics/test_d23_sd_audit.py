"""Synthetic FAT fixtures only: these tests do not execute D23 or Pokemon."""
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

from diagnostics.d23_sd_audit import TARGETS, audit, fingerprint

DATA = {"KANJI.K20": b"SYNTHETIC-KANJI-HEADER" + bytes(range(256)) * 280,
        "BPRJ_01.ks5": b"SYNTHETIC-READING-HEADER" + b"reading fixture\n" * 5000}
PATHS = {name: "/_gba/kotogba/" + name for name in TARGETS}


def make_image(path, data=DATA, fat_type=16):
    path.touch()
    fat = PyFat()
    fat.mkfs(str(path), fat_type=fat_type, size=(64 if fat_type == 32 else 16) * 1024 * 1024)
    fat.close()
    with PyFatFS(str(path)) as store:
        store.makedirs("/_gba/kotogba", recreate=True)
        for name, payload in data.items():
            store.writebytes(PATHS[name], payload)


def status(report, name="KANJI.K20"):
    return next(row for row in report["files"] if row["name"] == name)


class D23SDAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.reference = self.root / "reference"
        self.reference.mkdir()
        for name, data in DATA.items():
            (self.reference / name).write_bytes(data)
        self.image = self.root / "sd.img"
        make_image(self.image)

    def test_complete_multicluster_files(self):
        report = audit(self.image, self.reference)
        self.assertEqual(report["result"], "PASS_CONTENT_ONLY", report)
        self.assertEqual(report["path_validation"], "INCOMPLETE")
        self.assertFalse(report["in_game_reads_tested"])
        self.assertFalse(report["vocabulary_to_kanji_tested"])
        for row in report["files"]:
            self.assertEqual(row["actual"]["size"], len(DATA[row["name"]]))

    def test_same_header_but_corrupt_tail_is_not_accepted(self):
        damaged = DATA["KANJI.K20"][:-1] + b"!"
        self.assertEqual(damaged[:64], DATA["KANJI.K20"][:64])
        with PyFatFS(str(self.image)) as store:
            store.writebytes(PATHS["KANJI.K20"], damaged)
        report = audit(self.image, self.reference)
        self.assertEqual(report["result"], "FAIL")
        self.assertEqual(status(report)["status"], "BYTE_MISMATCH")
        self.assertTrue(status(report)["header_matches"])

    def test_truncated_file_is_distinct_from_missing(self):
        with PyFatFS(str(self.image)) as store:
            store.writebytes(PATHS["KANJI.K20"], DATA["KANJI.K20"][:128])
        report = audit(self.image, self.reference)
        self.assertEqual(status(report)["status"], "SIZE_MISMATCH")

    def test_missing_file_is_reported(self):
        with PyFatFS(str(self.image)) as store:
            store.remove(PATHS["KANJI.K20"])
        self.assertEqual(status(audit(self.image, self.reference))["status"], "FILE_MISSING")

    def test_wrong_expected_path_is_not_content_success(self):
        expected = {**PATHS, "KANJI.K20": "/another/KANJI.K20"}
        report = audit(self.image, self.reference, expected_paths=expected)
        self.assertEqual(report["result"], "FAIL")
        self.assertEqual(status(report)["status"], "PATH_MISMATCH")
        self.assertEqual(status(report)["locations"], [PATHS["KANJI.K20"]])

    def test_correct_paths_are_checked_separately(self):
        report = audit(self.image, self.reference, expected_paths=PATHS)
        self.assertEqual(report["result"], "PASS_CONTENT_AND_PATHS", report)
        self.assertFalse(report["in_game_reads_tested"])

    def test_fat_path_case_is_normalized(self):
        expected = {name: path.swapcase() for name, path in PATHS.items()}
        report = audit(self.image, self.reference, expected_paths=expected)
        self.assertEqual(report["result"], "PASS_CONTENT_AND_PATHS", report)

    def test_multiple_locations_are_not_silently_selected(self):
        with PyFatFS(str(self.image)) as store:
            store.makedirs("/backup", recreate=True)
            store.writebytes("/backup/KANJI.K20", DATA["KANJI.K20"])
        report = audit(self.image, self.reference)
        self.assertEqual(status(report)["status"], "MULTIPLE_LOCATIONS")
        report = audit(self.image, self.reference, expected_paths=PATHS)
        self.assertEqual(report["result"], "PASS_CONTENT_AND_PATHS", report)

    def test_different_reference_versions_are_rejected(self):
        backup = self.reference / "other"
        backup.mkdir()
        (backup / "KANJI.K20").write_bytes(b"other version")
        report = audit(self.image, self.reference)
        self.assertEqual(status(report)["status"], "REFERENCE_AMBIGUOUS")

    def test_identical_duplicate_references_are_allowed(self):
        backup = self.reference / "copy"
        backup.mkdir()
        (backup / "KANJI.K20").write_bytes(DATA["KANJI.K20"])
        report = audit(self.image, self.reference)
        self.assertEqual(report["result"], "PASS_CONTENT_ONLY", report)
        self.assertEqual(len(status(report)["reference"]), 2)

    def test_missing_reference_is_not_an_image_error(self):
        (self.reference / "KANJI.K20").unlink()
        self.assertEqual(status(audit(self.image, self.reference))["status"], "REFERENCE_MISSING")

    def test_empty_reference_cannot_validate_an_empty_package(self):
        (self.reference / "KANJI.K20").write_bytes(b"")
        with PyFatFS(str(self.image)) as store:
            store.writebytes(PATHS["KANJI.K20"], b"")
        report = audit(self.image, self.reference)
        self.assertEqual(status(report)["status"], "REFERENCE_EMPTY")

    def test_zip_reference_is_read_without_extraction(self):
        archive = self.root / "reference.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as store:
            for name, payload in DATA.items():
                store.writestr("approved/package/" + name, payload)
        before = hashlib.sha256(archive.read_bytes()).hexdigest()
        report = audit(self.image, archive)
        self.assertEqual(report["result"], "PASS_CONTENT_ONLY", report)
        self.assertEqual(before, hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertFalse((self.root / "approved").exists())

    def test_audit_does_not_change_the_image_bytes(self):
        before = hashlib.sha256(self.image.read_bytes()).hexdigest()
        audit(self.image, self.reference)
        after = hashlib.sha256(self.image.read_bytes()).hexdigest()
        self.assertEqual(before, after)

    def test_invalid_image_returns_structured_error(self):
        self.image.write_bytes(b"not FAT" * 128)
        report = audit(self.image, self.reference)
        self.assertEqual(report["result"], "ERROR")
        self.assertEqual(report["stage"], "fat_image")
        self.assertIn("error", report)

    def test_partition_offset_is_explicit(self):
        partitioned = self.root / "partitioned.img"
        offset = 1024 * 1024
        with partitioned.open("wb") as target, self.image.open("rb") as source:
            target.write(b"\0" * offset)
            shutil.copyfileobj(source, target)
        report = audit(partitioned, self.reference, offset=offset)
        self.assertEqual(report["result"], "PASS_CONTENT_ONLY", report)
        self.assertEqual(audit(partitioned, self.reference)["result"], "ERROR")
        self.assertEqual(audit(self.image, self.reference, offset=-1)["result"], "ERROR")

    def test_fat32_image(self):
        fat32 = self.root / "sd32.img"
        make_image(fat32, fat_type=32)
        report = audit(fat32, self.reference, expected_paths=PATHS)
        self.assertEqual(report["result"], "PASS_CONTENT_AND_PATHS", report)

    def test_short_stream_is_an_incomplete_read(self):
        with self.assertRaisesRegex(OSError, "Incomplete read"):
            fingerprint(io.BytesIO(b"short"), 100)

    def test_cli_json_and_exit_code(self):
        command = [sys.executable, "diagnostics/d23_sd_audit.py", "--image", str(self.image),
                   "--reference", str(self.reference)]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["result"], "PASS_CONTENT_ONLY")
        (self.reference / "KANJI.K20").write_bytes(b"wrong")
        completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(completed.returncode, 1)
        self.assertEqual(json.loads(completed.stdout)["result"], "FAIL")

    def test_report_cannot_overwrite_input_image(self):
        before = hashlib.sha256(self.image.read_bytes()).hexdigest()
        command = [sys.executable, "diagnostics/d23_sd_audit.py", "--image", str(self.image),
                   "--reference", str(self.reference), "--report", str(self.image)]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(before, hashlib.sha256(self.image.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main(verbosity=2)
