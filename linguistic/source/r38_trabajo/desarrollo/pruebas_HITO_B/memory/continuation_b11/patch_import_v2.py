#!/usr/bin/env python3
"""One reviewed B11 union with exact source bounds, preserving v1 unchanged.

The historical importer maps partial display units to their whole raw unit.
For this approved union that would include the preceding small tsu. This
versioned channel uses the exact two predecessor spans plus their literal
space, with a finite contract and unchanged transactional import machinery.
"""
from pathlib import Path
import argparse
import copy
import fcntl
import json
import sys
import unicodedata

HERE = Path(__file__).resolve().parent
MEM = HERE.parent
if str(MEM) not in sys.path:
    sys.path.insert(0, str(MEM))
import memory as m

SCHEMA = 'kotogba.exact_union_import/2'
RESULT_SCHEMA = 'kotogba.patch_import/2'
PACKET_ID = 'patches:B11-v2-reviewed-union-exact-raw:20261025'
HISTORICAL_MEMORY_SHA256 = 'ed5337c45884b852446be7c195e2ed8ae7b9f305e4dd3a4e67a2a6dbb603ae99'
HISTORICAL_RUNTIME_CARDS_SHA256 = 'd1876d6358506d89e5ff59d3db4fd23f92d659a48b84a2683eda9dbc75241e92'
REVIEW_FILE_SHA256 = '07e4221c81ac2f67dd016f7fea37d1ad3f7d5b6eac97c306444af2541ea1d2a6'
REVIEW_CANONICAL_SHA256 = '83a12a0f99af5fce074c0ee425efe218b24fe8a1698235177e189cce0b1f389a'
SOURCE_FILE_SHA256 = '9a288be133fd720449d0ce984992b6fa8b5c13a8769e910036ebed961783afcf'
DECISION_SHA256 = '63a902230860c9a17feac02d2aec7d01409b0228c7e0f282e06476a20d839f45'
RAW_SHA256 = '990c7b24c8a5e9b70b2e9aa42acb22534cd82abe9df1a33147340952889f07b7'
JP_SHA256 = '55eda8037b1033cd8d3ffc04015a77c38c69d22c6d5a9ddafe426bbc1085e4da'
BASE_SHA256 = 'd3fbe3d59d3dd65dc0c9f6436fcd2c1851df375a74aba9a92c84f5a4571542d9'
DATABASE_ID = 'memory:27d276afcdc1257ae9cb8448a66b1cddba8bcc68d3adf89eb0ba0a12d3dc21c9'
DID = 'bprj1:BPRJ1_168C11'
PACKET_KEYS = {'schema', 'packet_id', 'game_id', 'base_sha256',
               'expected_memory_revision', 'expected_memory_runtime_revision',
               'review_binding', 'source_binding', 'items'}
ITEM_KEYS = {'decision', 'raw_start', 'raw_end', 'document_raw_sha256',
             'document_japanese_sha256', 'original_unit_evidence',
             'before_adjacent_span', 'outside_spans_sha256'}
UNIT_EVIDENCE = [
    {'ordinal': 26, 'raw_start': 47, 'raw_end': 49, 'display_start': 123, 'display_end': 129},
    {'ordinal': 27, 'raw_start': 49, 'raw_end': 50, 'display_start': 129, 'display_end': 132},
    {'ordinal': 28, 'raw_start': 50, 'raw_end': 52, 'display_start': 132, 'display_end': 138},
]


def source_inputs():
    m.require(m.file_sha(MEM / 'memory.py') == HISTORICAL_MEMORY_SHA256 and
              m.file_sha(MEM / 'runtime_cards.py') == HISTORICAL_RUNTIME_CARDS_SHA256,
              'Historical import/catalog source differs')
    review_path = MEM / 'continuation_b11_staging/SECOND_READ_NOT_IMPORTED.json'
    source_path = MEM / 'continuation_b11_staging/patches_1_PROPOSED_NOT_IMPORTED.json'
    m.require(m.file_sha(review_path) == REVIEW_FILE_SHA256 and
              m.file_sha(source_path) == SOURCE_FILE_SHA256, 'Reviewed source files changed')
    review = json.loads(review_path.read_text())
    source = json.loads(source_path.read_text())
    m.require(m.digest(review) == REVIEW_CANONICAL_SHA256 and len(review['items']) == 100,
              'Other reviewed B11 packet')
    m.require(len(source['patches']) == 1 and m.digest(source['patches'][0]) == DECISION_SHA256,
              'Other semantic union decision')
    return review, source['patches'][0]


def bindings(review):
    return ({'packet_id': review['packet_id'], 'packet_sha256': REVIEW_CANONICAL_SHA256,
             'file_sha256': REVIEW_FILE_SHA256, 'adopted_memory_revision': 38},
            {'file_sha256': SOURCE_FILE_SHA256, 'decision_sha256': DECISION_SHA256})


def exact_source(c, item, decision):
    """Read-only proof of this finite display/raw mapping and untouched neighbour."""
    m.require(set(item) == ITEM_KEYS and m.digest(decision) == DECISION_SHA256,
              'Exact-union item or semantic preimage changed')
    m.require((decision['source_offset'], decision['start'], decision['end'], decision['visible']) ==
              (0x168C11, 126, 138, 'て　たら'), 'Other reviewed interval')
    m.require(type(item['raw_start']) is int and type(item['raw_end']) is int and
              (item['raw_start'], item['raw_end']) == (48, 52), 'Exact source range must be48:52')
    d = c.execute('SELECT * FROM koto_documents WHERE document_id=?', (DID,)).fetchone()
    m.require(d is not None and m.digest(d['raw_source']) == item['document_raw_sha256'] == RAW_SHA256 and
              m.digest(d['japanese'].encode()) == item['document_japanese_sha256'] == JP_SHA256,
              'Original source/transcription differs')
    jp = d['japanese'].encode()
    m.require(jp[126:138].decode() == 'て　たら' and
              bytes(d['raw_source'][48:52]) == bytes.fromhex('13001027') and
              jp[129:132].decode() == '　' and bytes(d['raw_source'][49:50]) == b'\x00',
              'Literal auxiliary/space correspondence differs')
    units = [dict(x) for x in c.execute(
        'SELECT ordinal,raw_start,raw_end,display_start,display_end FROM koto_units '
        'WHERE document_id=? AND display_start<138 AND display_end>126 ORDER BY ordinal', (DID,))]
    m.require(units == item['original_unit_evidence'] == UNIT_EVIDENCE,
              'Original unit mapping differs')
    spans = m._active_spans(c, DID)
    before = [s for s in spans if s['display_byte_start'] < 138 and 126 < s['display_byte_end']]
    m.require(before == decision['before'] and len(before) == 2,
              'Union predecessor preimage changed')
    m.require(min(s['raw_start'] for s in before) == 48 and max(s['raw_end'] for s in before) == 52,
              'Predecessors no longer prove exact source endpoints')
    outside = [s for s in spans if not (s['display_byte_start'] < 138 and 126 < s['display_byte_end'])]
    adjacent = [s for s in outside if (s['display_byte_start'], s['display_byte_end']) == (120, 126)]
    m.require(len(adjacent) == 1 and adjacent[0] == item['before_adjacent_span'] and
              (adjacent[0]['raw_start'], adjacent[0]['raw_end']) == (45, 48),
              'Adjacent pickup stem differs')
    m.require(m.digest(outside) == item['outside_spans_sha256'] and
              not any(s['raw_start'] < 52 and 48 < s['raw_end'] for s in outside),
              'Exact union would claim another active source span')
    m.require(c.execute('SELECT 1 FROM koto_occurrences WHERE occurrence_id=?',
                        (m.occurrence_id(DID, 126, 138),)).fetchone() is None,
              'A historical identity already occupies the new union')
    return d, before, outside


def contract(c, packet, review):
    m.require(set(packet) == PACKET_KEYS and packet['schema'] == SCHEMA and
              packet['packet_id'] == PACKET_ID and packet['game_id'] == 'bprj1' and
              packet['base_sha256'] == BASE_SHA256 and
              packet['expected_memory_revision'] == 38 and
              packet['expected_memory_runtime_revision'] == 26,
              'Other exact-union contract')
    m.require(type(packet['expected_memory_revision']) is int and
              type(packet['expected_memory_runtime_revision']) is int,
              'Noninteger version contract')
    rb, sb = bindings(review)
    m.require(packet['review_binding'] == rb and packet['source_binding'] == sb and
              isinstance(packet['items'], list) and len(packet['items']) == 1,
              'Other reviewed dependency or union count')
    m.require(m.meta(c, 'database_id') == DATABASE_ID, 'Other memory identity')
    g = c.execute('SELECT * FROM koto_games WHERE game_id=?', ('bprj1',)).fetchone()
    m.require(g and g['base_sha256'] == BASE_SHA256 and
              (g['revision'], g['runtime_revision']) == (38, 26), 'Stale exact-union authority')
    adopted = m._existing_import(c, review['packet_id'], REVIEW_CANONICAL_SHA256)
    m.require(adopted and adopted['revision'] == 38 and adopted['items'] == 100,
              'The exact reviewed100 dependency is not adopted')
    stored = c.execute('SELECT packet_json FROM koto_packets WHERE packet_id=?',
                       (review['packet_id'],)).fetchone()
    m.require(stored and m.digest(json.loads(stored[0])) == REVIEW_CANONICAL_SHA256,
              'Stored reviewed dependency changed')
    return g


def prepare(db_path, output):
    review, decision = source_inputs()
    db_path = Path(db_path)
    before_hash = m.file_sha(db_path)
    with m.connect(db_path, readonly=True) as c:
        c.execute('BEGIN')
        spans = m._active_spans(c, DID)
        outside = [s for s in spans if not (s['display_byte_start'] < 138 and 126 < s['display_byte_end'])]
        adjacent = [s for s in outside if (s['display_byte_start'], s['display_byte_end']) == (120, 126)]
        m.require(len(adjacent) == 1, 'Missing adjacent pickup stem')
        rb, sb = bindings(review)
        packet = dict(schema=SCHEMA, packet_id=PACKET_ID, game_id='bprj1', base_sha256=BASE_SHA256,
                      expected_memory_revision=38, expected_memory_runtime_revision=26,
                      review_binding=rb, source_binding=sb,
                      items=[dict(decision=copy.deepcopy(decision), raw_start=48, raw_end=52,
                                  document_raw_sha256=RAW_SHA256, document_japanese_sha256=JP_SHA256,
                                  original_unit_evidence=copy.deepcopy(UNIT_EVIDENCE),
                                  before_adjacent_span=adjacent[0], outside_spans_sha256=m.digest(outside))])
        contract(c, packet, review)
        exact_source(c, packet['items'][0], decision)
        c.rollback()
    m.require(m.file_sha(db_path) == before_hash, 'Preparation changed the authority')
    m.require(not Path(output).exists(), 'Use a new exact-union output')
    m.write_json(output, packet)
    return {'schema': SCHEMA, 'status': 'PREPARED_NOT_IMPORTED', 'packet_id': PACKET_ID,
            'packet_sha256': m.digest(packet), 'packet_file_sha256': m.file_sha(output),
            'database_sha256_before': before_hash, 'database_sha256_after': m.file_sha(db_path),
            'canonical_writes': 0, 'raw_start': 48, 'raw_end': 52}


def apply_patches(db_path, patch_path, game='bprj1', reviewer='Damián', fault_hook=None):
    """Import this finite v2 union with v1 backup, history and rollback guards."""
    review, original_decision = source_inputs()
    packet = json.loads(Path(patch_path).read_text(encoding='utf-8'))
    m.require(game == 'bprj1' and isinstance(reviewer, str) and reviewer.strip(), 'Other game or missing reviewer')
    m.require(isinstance(packet, dict) and set(packet) == PACKET_KEYS and packet['schema'] == SCHEMA and
              packet['packet_id'] == PACKET_ID, 'Other exact-union schema or identity')
    ph, pid = m.digest(packet), packet['packet_id']
    db_path = Path(db_path)
    with db_path.with_name(db_path.name + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        conn = m.connect(db_path)
        try:
            m.check_schema(conn)
            conn.execute('BEGIN IMMEDIATE')
            prior = m._existing_import(conn, pid, ph)
            if prior:
                conn.rollback()
                return prior
            grow = contract(conn, packet, review)
            item = packet['items'][0]
            patch = item['decision']
            m.require(patch == original_decision, 'Reviewed union semantics changed')
            d, before, outside = exact_source(conn, item, patch)
            document_before = dict(d)
            lo, hi = patch['start'], patch['end']
            jp = d['japanese'].encode()
            m.require(all(lo <= s['display_byte_start'] < s['display_byte_end'] <= hi for s in before),
                      'A correction cuts an old occurrence')
            m.require(not any(s['card']['type'] == 4 for s in before), 'Player variables cannot be rewritten')
            m.require(isinstance(patch['replacements'], list) and len(patch['replacements']) == 1,
                      'Only the one reviewed union replacement is admitted')
            after = []
            for replacement in patch['replacements']:
                rlo, rhi = replacement['start'], replacement['end']
                m.require(type(rlo) is int and type(rhi) is int and lo <= rlo < rhi <= hi,
                          'Replacement outside the reviewed interval')
                m.require(jp[rlo:rhi].decode() == replacement['visible'] and (rlo, rhi) == (126, 138),
                          'Different replacement surface')
                rc = {k: replacement['lemma' if k == 'surface' else k] for k in m.REPLACEMENT_KEYS}
                m.require(rc['type'] == 2, 'This reviewed union is black grammar')
                after.append((rlo, rhi, item['raw_start'], item['raw_end'], rc))
            at = 0
            for char in d['japanese']:
                end = at + len(char.encode())
                if lo <= at and end <= hi and unicodedata.category(char)[0] in 'LN':
                    covered_before = any(s['display_byte_start'] <= at < end <= s['display_byte_end'] for s in before)
                    if covered_before:
                        m.require(sum(a <= at < end <= z for a, z, *_ in after) == 1,
                                  'Correction leaves a linked glyph inaccessible')
                at = end
            backup = m._backup(conn, db_path, ph)
            revision = grow['revision'] + 1
            result = {'schema': RESULT_SCHEMA, 'packet_id': pid, 'packet_sha256': ph,
                      'game_id': game, 'revision': revision, 'runtime_revision': 27,
                      'patches': 1, 'backup': backup, 'already_applied': False,
                      'exact_raw_range': [48, 52], 'exact_display_range': [126, 138],
                      'adjacent_source_span_preserved': [45, 48],
                      'review_dependency_sha256': REVIEW_CANONICAL_SHA256,
                      'adapter_sha256': m.file_sha(Path(__file__)), 'existing_source_units_modified': False}
            conn.execute('INSERT INTO koto_imports VALUES(?,?,?,?)', (pid, ph, game, m.canonical(result)))
            m._store_packet(conn, pid, packet)
            changed_before = {}
            for old in before:
                oid = m.occurrence_id(DID, old['display_byte_start'], old['display_byte_end'])
                row = m._occurrence(conn, oid)
                changed_before[oid] = m._state(row)
                conn.execute('UPDATE koto_occurrences SET active=0,revision=revision+1 WHERE occurrence_id=?', (oid,))
            for rlo, rhi, rawlo, rawhi, replacement in after:
                card = m._new_card(conn, replacement)
                provenance = {'packet_id': pid, 'reviewer': reviewer, 'decision_id': patch['decision_id'],
                              'reason': patch['reason'], 'source_offset': d['source_offset'],
                              'exact_source_mapping': {'raw_start': rawlo, 'raw_end': rawhi,
                                                       'historical_coarse_raw_start_not_used': 47}}
                variant = m.register_card(conn, card, provenance)
                oid = m.occurrence_id(DID, rlo, rhi)
                ordinal = conn.execute('SELECT COALESCE(MAX(ordinal),-1)+1 FROM koto_occurrences WHERE document_id=?', (DID,)).fetchone()[0]
                changed_before[oid] = None
                conn.execute('INSERT INTO koto_occurrences VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                             (oid, DID, ordinal, rawlo, rawhi, rlo, rhi, 2, variant, variant, 1,
                              'reviewed', reviewer, provenance['reason'], m.canonical(provenance), 1))
            for oid, old_state in changed_before.items():
                m._log_change(conn, game, revision, pid, oid, old_state, m._state(m._occurrence(conn, oid)))
            if fault_hook:
                fault_hook('after_item', 1)
            conn.execute('UPDATE koto_games SET revision=?,runtime_revision=runtime_revision+1 WHERE game_id=?', (revision, game))
            m._validate_changed_documents(conn, game, [DID])
            m.require(dict(conn.execute('SELECT * FROM koto_documents WHERE document_id=?', (DID,)).fetchone()) == document_before,
                      'Original document changed')
            outside_after = [s for s in m._active_spans(conn, DID)
                             if not (s['display_byte_start'] < 138 and 126 < s['display_byte_end'])]
            m.require(outside_after == outside, 'An adjacent occurrence changed')
            new = m._occurrence(conn, m.occurrence_id(DID, 126, 138))
            m.require((new['raw_start'], new['raw_end'], new['display_start'], new['display_end']) == (48, 52, 126, 138),
                      'New union mapping changed')
            if fault_hook:
                fault_hook('before_commit', 1)
            conn.commit()
            return result
        except BaseException:
            conn.rollback()
            raise
        finally:
            conn.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    q = sub.add_parser('prepare')
    q.add_argument('--db', type=Path, required=True)
    q.add_argument('--output', type=Path, required=True)
    q.add_argument('--report', type=Path)
    q = sub.add_parser('apply')
    q.add_argument('--db', type=Path, required=True)
    q.add_argument('--packet', type=Path, required=True)
    q.add_argument('--reviewer', default='Damián')
    q.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = prepare(args.db, args.output) if args.command == 'prepare' else apply_patches(args.db, args.packet, reviewer=args.reviewer)
    if args.report:
        m.write_json(args.report, result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
