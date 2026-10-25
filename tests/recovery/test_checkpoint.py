"""Infrastructure tests use synthetic data only, never the game or private corpus."""
import concurrent.futures
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("checkpoint", ROOT / "tools/recovery/checkpoint.py")
cp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cp)


def packet(name="batch-001", ids=None):
    return {"schema": "kotogba.prepared-batch/1", "batch_id": name, "base_sha256": "a" * 64, "input_sha256": "b" * 64, "output_sha256": "c" * 64, "occurrence_ids": ids or ["synthetic:1"], "status": "prepared"}


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.journal = self.root / "journal"

    def test_create(self):
        self.assertEqual(cp.record(self.journal, packet()), "created")
        self.assertEqual(json.loads((self.journal / "batch-001.json").read_text()), packet())

    def test_idempotent(self):
        cp.record(self.journal, packet())
        self.assertEqual(cp.record(self.journal, packet()), "already_present")
        self.assertEqual(len(list(self.journal.glob("*.json"))), 1)

    def test_order_is_canonical(self):
        cp.record(self.journal, packet(ids=["b", "a"]))
        self.assertEqual(cp.record(self.journal, packet(ids=["a", "b"])), "already_present")

    def test_conflicting_preimage_rejected(self):
        cp.record(self.journal, packet())
        modified = packet()
        modified["input_sha256"] = "d" * 64
        with self.assertRaises(ValueError):
            cp.record(self.journal, modified)
        self.assertEqual(json.loads((self.journal / "batch-001.json").read_text()), packet())

    def test_overlap_rejected(self):
        cp.record(self.journal, packet())
        with self.assertRaises(ValueError):
            cp.record(self.journal, packet("batch-002"))

    def test_nonoverlap_accepted(self):
        cp.record(self.journal, packet())
        self.assertEqual(cp.record(self.journal, packet("batch-002", ["synthetic:2"])), "created")

    def test_duplicate_occurrences_rejected(self):
        with self.assertRaises(ValueError):
            cp.validate_receipt(packet(ids=["a", "a"]))

    def test_oversized_batch_rejected(self):
        with self.assertRaises(ValueError):
            cp.validate_receipt(packet(ids=[str(i) for i in range(101)]))

    def test_invalid_hash_rejected(self):
        value = packet()
        value["output_sha256"] = "not-a-hash"
        with self.assertRaises(ValueError):
            cp.validate_receipt(value)

    def test_path_traversal_rejected(self):
        with self.assertRaises(ValueError):
            cp.record(self.journal, packet("../bad"))
        self.assertFalse(self.journal.exists())

    def test_private_payload_rejected(self):
        value = packet()
        value["translation"] = "synthetic private text"
        with self.assertRaises(ValueError):
            cp.validate_receipt(value)

    def test_imported_status_rejected(self):
        value = packet()
        value["status"] = "imported"
        with self.assertRaises(ValueError):
            cp.validate_receipt(value)

    def test_interrupted_publication_no_truncated_final(self):
        with mock.patch.object(cp.os, "link", side_effect=OSError("simulated interruption")):
            with self.assertRaises(OSError):
                cp.record(self.journal, packet())
        self.assertEqual(list(self.journal.glob("*.json")), [])
        self.assertEqual(cp.record(self.journal, packet()), "created")

    def test_concurrent_identical_writers(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: cp.record(self.journal, packet()), range(8)))
        self.assertEqual(results.count("created"), 1)
        self.assertEqual(results.count("already_present"), 7)

    def test_corrupt_existing_receipt_blocks_new_batch(self):
        self.journal.mkdir()
        (self.journal / "broken.json").write_text("{truncated")
        with self.assertRaises(ValueError):
            cp.record(self.journal, packet())
        self.assertFalse((self.journal / "batch-001.json").exists())

    def test_symlink_journal_rejected(self):
        destination = self.root / "other"
        destination.mkdir()
        self.journal.symlink_to(destination, target_is_directory=True)
        with self.assertRaises(ValueError):
            cp.record(self.journal, packet())

    def make_zip(self, member="source/example.txt", symlink=False):
        path = self.root / "sample.zip"
        with zipfile.ZipFile(path, "w") as archive:
            info = zipfile.ZipInfo(member)
            if symlink:
                info.create_system = 3
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, b"synthetic test only")
        return path, {"bytes": path.stat().st_size, "sha256": cp.digest(path)}

    def test_archive_integrity(self):
        path, expected = self.make_zip()
        self.assertTrue(cp.verify_archive(path, expected)["passed"])
        self.assertFalse((self.root / "source").exists())

    def test_archive_size_mismatch(self):
        path, expected = self.make_zip()
        expected["bytes"] += 1
        with self.assertRaises(ValueError):
            cp.verify_archive(path, expected)

    def test_archive_hash_mismatch(self):
        path, expected = self.make_zip()
        expected["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            cp.verify_archive(path, expected)

    def test_archive_traversal_rejected(self):
        path, expected = self.make_zip("../outside.txt")
        with self.assertRaises(ValueError):
            cp.verify_archive(path, expected)

    def test_archive_symlink_rejected(self):
        path, expected = self.make_zip(symlink=True)
        with self.assertRaises(ValueError):
            cp.verify_archive(path, expected)

    def test_truncated_archive_rejected(self):
        path, expected = self.make_zip()
        path.write_bytes(path.read_bytes()[:20])
        expected = {"bytes": path.stat().st_size, "sha256": cp.digest(path)}
        with self.assertRaises(zipfile.BadZipFile):
            cp.verify_archive(path, expected)

    def test_repository_state_valid_but_not_releasable(self):
        state = json.loads((ROOT / "project/STATE.json").read_text())
        cp.validate_state(state)
        with self.assertRaises(ValueError):
            cp.release_gate(state)

    def test_false_installable_rejected(self):
        state = json.loads((ROOT / "project/STATE.json").read_text())
        state["installable"] = True
        with self.assertRaises(ValueError):
            cp.validate_state(state)

    def test_string_boolean_rejected(self):
        state = json.loads((ROOT / "project/STATE.json").read_text())
        state["current_sources_imported"] = "false"
        with self.assertRaises(ValueError):
            cp.validate_state(state)


if __name__ == "__main__":
    unittest.main()
