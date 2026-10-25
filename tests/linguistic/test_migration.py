"""Public B12 checks: code, imports and synthetic invariants only."""
from pathlib import Path
import importlib.util
import sqlite3
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LINGUISTIC = ROOT / 'linguistic'
PROJECT = LINGUISTIC / 'source/r38_trabajo'
MEMORY = PROJECT / 'desarrollo/pruebas_HITO_B/memory'
sys.path.insert(0, str(MEMORY))
from continuation_b12 import finite_contract_v4 as finite
from continuation_b12 import export_runtime_v4 as exporter
from continuation_b12 import publish_snapshot_v4 as publisher
import memory

spec = importlib.util.spec_from_file_location('verify_linguistic_sources', LINGUISTIC / 'verify.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


def specimen(reverse=False):
    db = sqlite3.connect(':memory:')
    db.row_factory = sqlite3.Row
    db.executescript('''
        CREATE TABLE koto_probe(id INTEGER PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE entries(id INTEGER PRIMARY KEY, payload TEXT NOT NULL);
        CREATE TABLE forms(form TEXT NOT NULL, entry_id INTEGER NOT NULL, kind TEXT NOT NULL,
                           UNIQUE(form,entry_id,kind));
        CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE unordered_values(value COLLATE NOCASE, extra);
    ''')
    data = {
        'koto_probe': [(2, 'two'), (1, 'one')],
        'entries': [(2, 'entry two'), (1, 'entry one')],
        'forms': [('alpha', 2, 'reading'), ('beta', 1, 'written'), ('alpha', 1, 'reading')],
        'metadata': [('version', '1'), ('dictionary', 'synthetic')],
        'unordered_values': [('a', None), ('A', None), (1, 'number'), (1.0, 'number'),
                             (b'a', None), (None, 3), ('duplicate', 4), ('duplicate', 4)],
    }
    for table, rows in data.items():
        db.executemany('INSERT INTO ' + table + ' VALUES(' + ','.join('?' for _ in rows[0]) + ')',
                       list(reversed(rows)) if reverse else rows)
    return db


class LinguisticMigrationTests(unittest.TestCase):
    def test_all_public_sources_preserve_original_bytes(self):
        result = verify.verify()
        self.assertTrue(result['passed'])
        self.assertEqual((result['memory_revision'], result['runtime_revision']), (50, 36))
        self.assertFalse(result['private_inputs_verified'])

    def test_entrypoints_help_without_private_inputs(self):
        for script in ('memory.py', 'incremental_queue.py', 'continuation_b12/adopt_b12.py',
                       'continuation_b12/export_runtime_v4.py', 'continuation_b12/publish_snapshot_v4.py'):
            with self.subTest(script=script):
                result = subprocess.run([sys.executable, str(MEMORY / script), '--help'],
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('usage:', result.stdout)

    def test_finite_state_includes_dictionary_tables_and_preserves_order_independence(self):
        first, second = specimen(), specimen(reverse=True)
        try:
            self.assertTrue({'entries', 'forms', 'metadata'} <= set(finite.tables(first)))
            before = {table: finite.table_state(first, table) for table in finite.tables(first)}
            self.assertEqual(before, {table: finite.table_state(second, table) for table in finite.tables(second)})
            second.execute("UPDATE entries SET payload='changed' WHERE id=1")
            self.assertNotEqual(before['entries'], finite.table_state(second, 'entries'))
            second.execute("DELETE FROM unordered_values WHERE rowid=(SELECT MAX(rowid) FROM unordered_values)")
            self.assertNotEqual(before['unordered_values'], finite.table_state(second, 'unordered_values'))
        finally:
            first.close(); second.close()

    def test_outputs_cannot_alias_database_or_staging_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            db = root / 'toy.sqlite'
            db.write_bytes(b'synthetic only')
            with self.assertRaises(memory.ValidationError):
                exporter.validate_output_paths(db, db)
            with self.assertRaises(memory.ValidationError):
                publisher.validate_paths(db, db)
            stage = root / 'toy.sqlite.writing'
            stage.write_bytes(b'preexisting source')
            with self.assertRaises(memory.ValidationError):
                exporter.validate_output_paths(stage, db)
            self.assertEqual(db.read_bytes(), b'synthetic only')
            self.assertEqual(stage.read_bytes(), b'preexisting source')

    def test_public_exporter_refuses_missing_private_contract(self):
        self.assertFalse(exporter.CONTRACT_FILE.exists())
        with self.assertRaises((FileNotFoundError, memory.ValidationError)):
            exporter._contract(None, None)


if __name__ == '__main__':
    unittest.main()
