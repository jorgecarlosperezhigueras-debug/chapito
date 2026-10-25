"""Negative tests for the boundary between public sources and private builds."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import warnings
import zipfile

SPEC = importlib.util.spec_from_file_location(
    "b19", Path(__file__).resolve().parents[2] / "tools/build/b19.py")
b19 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(b19)


class PrivateBuildInputTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def make_bundle(self, contents):
        path = self.root / "inputs.zip"
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(path, "w") as archive:
                for name, data in contents:
                    archive.writestr(name, data)
        return path

    def expected(self, bundle, entries):
        result = {"archive": b19.digest(bundle), "entries": {}}
        for name, data in entries:
            result["entries"][name] = {
                "bytes": len(data), "sha256": b19.hashlib.sha256(data).hexdigest()}
        return result

    def test_exact_bundle_is_accepted(self):
        entries = [("project/source.c", b"source")]
        path = self.make_bundle(entries)
        b19.verify_bundle(path, self.expected(path, entries))

    def test_old_or_corrupt_bundle_fails_before_unpack(self):
        entries = [("project/source.c", b"source")]
        path = self.make_bundle(entries)
        lock = self.expected(path, entries)
        with path.open("ab") as stream:
            stream.write(b"unexpected")
        with self.assertRaisesRegex(ValueError, "B19 lock"):
            b19.verify_bundle(path, lock)

    def test_changed_member_fails_even_when_outer_archive_hash_matches(self):
        path = self.make_bundle([("project/source.c", b"wrong!")])
        lock = self.expected(path, [("project/source.c", b"source")])
        with self.assertRaisesRegex(ValueError, "B19 lock"):
            b19.verify_bundle(path, lock)

    def test_unlisted_extra_file_fails(self):
        path = self.make_bundle([("project/source.c", b"ok"), ("unlisted", b"extra")])
        lock = self.expected(path, [("project/source.c", b"ok")])
        with self.assertRaisesRegex(ValueError, "member set"):
            b19.verify_bundle(path, lock)

    def test_duplicate_member_fails(self):
        path = self.make_bundle([("project/source.c", b"ok"), ("project/source.c", b"ok")])
        lock = self.expected(path, [("project/source.c", b"ok")])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            b19.verify_bundle(path, lock)

    def test_parent_traversal_member_fails(self):
        entries = [("../outside", b"bad")]
        path = self.make_bundle(entries)
        with self.assertRaisesRegex(ValueError, "Unsafe path"):
            b19.verify_bundle(path, self.expected(path, entries))

    def test_zip_symlink_fails(self):
        path = self.root / "inputs.zip"
        info = zipfile.ZipInfo("project/link")
        info.create_system = 3
        info.external_attr = 0o120777 << 16
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(info, b"target")
        lock = self.expected(path, [("project/link", b"target")])
        with self.assertRaisesRegex(ValueError, "Non-file"):
            b19.verify_bundle(path, lock)

    def test_public_source_symlink_is_rejected(self):
        source = self.root / "source.c"
        source.write_text("source")
        (self.root / "link.c").symlink_to(source)
        with self.assertRaisesRegex(ValueError, "linked input"):
            b19.contained_file(self.root, "link.c")

    def test_parent_directory_symlink_cannot_escape_public_root(self):
        current = self.root / "current"
        current.mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "source.c").write_text("source")
        (current / "nested").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "escapes"):
            b19.contained_file(current, "nested/source.c")

    def test_public_source_changes_are_detected(self):
        current = self.root / "current"
        current.mkdir()
        source = current / "source.c"
        source.write_text("B19")
        lock = {"source_files": {"source.c": {**b19.digest(source),
                "storage": "public", "path": "source.c"}},
                "public_source_files": 1, "private_source_files": 0}
        b19.verify_sources(self.root, lock)
        source.write_text("B18")
        with self.assertRaisesRegex(ValueError, "B19 lock"):
            b19.verify_sources(self.root, lock)

    def test_invalid_path_spellings_fail(self):
        for name in ["/etc/passwd", "a/../b", "./source", "a//b", "C:/data", "a\\b"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                b19.safe_name(name)


if __name__ == "__main__":
    unittest.main()
