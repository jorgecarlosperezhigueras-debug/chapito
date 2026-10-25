#!/usr/bin/env python3
"""Five finite B12 inline-dispatch repairs; B10/B11 stay immutable.

The historical codec may emit a nonempty inline table while retaining the
original document's bit4=0. This adapter admits only the five exact reviewed
B12 preimages. It changes bit4 at byte60, then uses B11's unchanged strict
consumer-equivalent gate. B11's own 3D9703 repair remains delegated to B11.
No SQLite mutation, semantic approval, span edit, or general repair occurs.
"""
from pathlib import Path
import json
import struct
import sys

HERE = Path(__file__).resolve().parent
MEM = HERE.parent
PROJECT = MEM.parents[2]
if str(MEM) not in sys.path:
    sys.path.insert(0, str(MEM))
import memory as m
from continuation_b11 import export_runtime_v3 as b11
from continuation_b10 import variable_links_v2 as marker_v2

CONTRACT_FILE = HERE / 'HEADER_DISPATCH_B12_CONTRACT.json'
CONTRACT_SHA256 = '509d0c96f46f336f27495772806b4eabddef4f028bb0fcad32ae1d0588d2de11'
SCHEMA = 'kotogba.B12.finite_header_dispatch/1'
REPAIR_SOURCES = frozenset((0x3D920F, 0x3DDB94, 0x3DDBB8, 0x3DDBC0, 0x3DDBCC))
ALL_REPAIR_SOURCES = REPAIR_SOURCES | {b11.REPAIR_SOURCE}
CONSUMER_HEADER_REFERENCE = dict(
    b11.CONSUMER_HEADER_REFERENCE,
    portable_path='continuation_b12/source_evidence/KotoGbaLinguistic.inc')
require = m.require


def _binding_path(item):
    require(item['root'] in ('memory', 'project'), 'Unknown header dependency root')
    rel = Path(item['path'])
    require(not rel.is_absolute() and '..' not in rel.parts, 'Header dependency escapes root')
    root = MEM if item['root'] == 'memory' else PROJECT
    path = (root / rel).resolve()
    require(path.is_relative_to(root.resolve()) and path.is_file(), 'Missing header dependency')
    return path


def load_contract():
    require(m.file_sha(CONTRACT_FILE) == CONTRACT_SHA256, 'Finite B12 header contract changed or unsealed')
    contract = json.loads(CONTRACT_FILE.read_text(encoding='utf-8'))
    require(contract['schema'] == SCHEMA and contract['status'] == 'FINITE_PREIMAGES_REVIEWED',
            'Unexpected B12 header contract')
    entries = contract['documents']
    require(len(entries) == 5 and {e['source_offset'] for e in entries} == REPAIR_SOURCES,
            'Finite B12 header inventory differs from the five reviewed documents')
    require(sum(len(e['evidence']['occurrences']) for e in entries) == 8,
            'Finite B12 header inventory no longer has eight reviewed links')
    require(contract['inherited_b11']['source_offset'] == b11.REPAIR_SOURCE and
            contract['inherited_b11']['preimage_sha256'] == b11.REPAIR_PREIMAGE_SHA256,
            'Inherited B11 repair authority changed')
    require(Path(m.__file__).resolve() == (MEM / 'memory.py').resolve() and
            Path(b11.__file__).resolve() == (MEM / 'continuation_b11/export_runtime_v3.py').resolve() and
            Path(marker_v2.__file__).resolve() == (MEM / 'continuation_b10/variable_links_v2.py').resolve(),
            'Header adapter imported a module outside the pinned project')
    for item in contract['bound_files']:
        require(m.file_sha(_binding_path(item)) == item['sha256'],
                'Finite B12 header dependency changed: ' + item['path'])
    return contract


def dependency_bindings():
    contract = load_contract()
    return [{'root': 'memory', 'path': 'continuation_b12/header_dispatch_b12.py'},
            {'root': 'memory', 'path': 'continuation_b12/HEADER_DISPATCH_B12_CONTRACT.json'}] + [
                {k: item[k] for k in ('root', 'path')} for item in contract['bound_files']]


def _json_row(row):
    return {k: ({'blob_hex': value.hex()} if isinstance(value, bytes) else value)
            for k, value in dict(row).items()}


def _rows(conn, sql, args=()):
    return [_json_row(r) for r in conn.execute(sql, args)]


def evidence_snapshot(conn, d):
    """Exact local state, including inactive occurrences and complete history.

    JSON text columns remain byte-exact strings. Packet JSON is bound both as
    stored bytes and as canonical JSON, and the actual adoption row is retained.
    This is a comparison helper, never an approval or contract generator.
    """
    did = d['document_id']
    document = _json_row(d)
    document['original_bytes'] = {'sha256': m.digest(d['original_bytes']), 'bytes': len(d['original_bytes'])}
    result = {'document': document}
    queries = {
        'units': ('SELECT * FROM koto_units WHERE document_id=? ORDER BY ordinal', (did,)),
        'teacher_notes': ('SELECT * FROM koto_teacher_notes WHERE document_id=? ORDER BY ordinal', (did,)),
        'document_cards': ('SELECT * FROM koto_document_cards WHERE document_id=? ORDER BY ordinal', (did,)),
        'runtime_cards': ('SELECT * FROM koto_runtime_cards WHERE document_id=? ORDER BY ordinal', (did,)),
        'occurrences': ('SELECT * FROM koto_occurrences WHERE document_id=? ORDER BY occurrence_id', (did,)),
        'translations': ('SELECT * FROM koto_document_translations WHERE document_id=? ORDER BY revision', (did,)),
        'changes': ('SELECT h.* FROM koto_changes h JOIN koto_occurrences o USING(occurrence_id) '
                    'WHERE o.document_id=? ORDER BY h.change_id', (did,)),
    }
    for key, (sql, args) in queries.items():
        result[key] = _rows(conn, sql, args)
    variant_ids = {r[k] for r in result['occurrences'] for k in ('variant_id', 'baseline_variant_id')}
    variant_ids.update(r['variant_id'] for key in ('document_cards', 'runtime_cards') for r in result[key])
    result['variants'] = []
    for vid in sorted(variant_ids):
        rows = _rows(conn, 'SELECT * FROM koto_card_variants WHERE variant_id=?', (vid,))
        require(len(rows) == 1, 'Missing exact header card variant')
        result['variants'].extend(rows)
    result['lexemes'] = []
    for value in sorted({v['lexeme_id'] for v in result['variants']}):
        result['lexemes'].extend(_rows(conn, 'SELECT * FROM koto_lexemes WHERE lexeme_id=?', (value,)))
    result['senses'] = []
    for value in sorted({v['sense_id'] for v in result['variants']}):
        result['senses'].extend(_rows(conn, 'SELECT * FROM koto_senses WHERE sense_id=?', (value,)))
    result['new_identities'] = []
    for value in sorted({v['runtime_identity'] for v in result['variants']}):
        result['new_identities'].extend(_rows(conn, 'SELECT * FROM koto_new_identities WHERE runtime_identity=?', (value,)))
    packet_ids = {row['packet_id'] for row in result['changes']}
    packet_ids.update(json.loads(o['provenance_json'])['packet_id'] for o in result['occurrences'])
    result['packets'] = []
    for pid in sorted(packet_ids):
        p = conn.execute('SELECT * FROM koto_packets WHERE packet_id=?', (pid,)).fetchone()
        imported = conn.execute('SELECT * FROM koto_imports WHERE packet_id=?', (pid,)).fetchone()
        require(p is not None and imported is not None, 'Missing adopted variable packet history')
        packet = json.loads(p['packet_json'])
        require(packet['packet_id'] == pid and m.digest(packet) == imported['packet_sha256'],
                'Stored variable packet differs from its adoption digest')
        items = [item for item in packet['items'] if item['source_offset'] == d['source_offset']]
        result['packets'].append({
            'packet_id': pid, 'schema': packet['schema'], 'reviewer': packet['reviewer'],
            'stored_json_sha256': m.digest(p['packet_json'].encode('utf-8')),
            'canonical_sha256': m.digest(packet), 'items_in_document': items,
            'import_row': _json_row(imported)})
    return result


def verify_evidence(conn, d, entry, contract=None):
    contract = load_contract() if contract is None else contract
    require(d['source_offset'] == entry['source_offset'] and d['source_end'] == entry['source_end'] and
            d['document_id'] == entry['document_id'], 'Finite header source/identity/range changed')
    g = conn.execute('SELECT game_id,base_sha256,revision,runtime_revision FROM koto_games WHERE game_id=?',
                     (d['game_id'],)).fetchone()
    require(g is not None and dict(g) == contract['game'] and
            m.meta(conn, 'database_id') == contract['database_id'], 'Other B12 header state or memory')
    observed = evidence_snapshot(conn, d)
    require(observed == entry['evidence'], 'Finite header source/units/card/history preimage changed')
    require(m.digest(d['original_bytes']) == d['source_sha256'] == entry['original_sha256'],
            'Finite header original document preimage changed')
    before = m.kl.document(d['original_bytes'])
    require(before['raw'] == d['raw_source'] and before['japanese'] == d['japanese'] and
            before['spans'] == before['cards'] == [] and observed['document_cards'] == [],
            'Finite B12 repair no longer starts from an original document without inline cards')
    require(observed['translations'] == [] and before['spanish'] == d['spanish'],
            'Unreviewed translation change in finite header document')
    require([{'raw_start': u['raw_start'], 'raw_end': u['raw_end'], 'display': u['display'],
              'reading': u['reading'], 'display_byte_start': u['display_start'],
              'display_byte_end': u['display_end']} for u in observed['units']] == before['units'] and
            before['notes'] == observed['teacher_notes'] == [],
            'Finite header units/notes differ from the original binary')
    mapping = marker_v2.marker_mapping(d)
    require([{'display': list(k), 'raw': list(v[:2]), 'visible': v[2]} for k, v in sorted(mapping.items())]
            == entry['marker_mapping'], 'Frozen lexer marker coordinates differ')
    spans = m._active_spans(conn, d['document_id'])
    cards = m.runtime_cards.cards(conn, d['document_id'])
    require(spans == entry['spans'] and cards == entry['cards'], 'Finite header spans or stable cards changed')
    require(len(spans) == len(cards) == len(observed['occurrences']) == len(observed['changes']),
            'Finite header link/creation/card inventory differs')
    variants = {v['variant_id']: v for v in observed['variants']}
    imports = {p['packet_id']: p for p in observed['packets']}
    for o in observed['occurrences']:
        require(o['active'] == 1 and o['status'] == 'reviewed' and o['flags'] == 1 and o['revision'] == 1 and
                o['variant_id'] == o['baseline_variant_id'], 'Finite header link is no longer the approved creation')
        provenance = json.loads(o['provenance_json'])
        p = imports[provenance['packet_id']]
        candidates = [item for item in p['items_in_document']
                      if (item['raw_start'], item['raw_end'], item['display_byte_start'], item['display_byte_end']) ==
                         (o['raw_start'], o['raw_end'], o['display_start'], o['display_end'])]
        require(len(candidates) == 1, 'Finite header link does not match one adopted item')
        item = candidates[0]
        require(item['before'] == [] and item['raw_source_sha256'] == m.digest(d['raw_source']) and
                mapping.get((o['display_start'], o['display_end'])) ==
                (o['raw_start'], o['raw_end'], item['visible']), 'Finite header marker/source preimage differs')
        require(any(u['display_start'] <= o['display_start'] < o['display_end'] <= u['display_end'] and
                    u['raw_start'] <= o['raw_start'] < o['raw_end'] <= u['raw_end'] for u in observed['units']),
                'Finite header marker has no exact original covering unit')
        card = json.loads(variants[o['variant_id']]['card_json'])
        expected = {key: (value if key == 'type' else m.kl.wrap(value, key == 'reading'))
                    for key, value in item['replacement'].items()}
        require({key: value for key, value in card.items() if key != 'identity'} == expected and
                card['type'] == 4 and card['surface'] == item['visible'] and card['reading'] == '',
                'Finite header card differs from its approved wrapped replacement')
        changes = [h for h in observed['changes'] if h['occurrence_id'] == o['occurrence_id']]
        receipt = json.loads(p['import_row']['result_json'])
        require(len(changes) == 1 and changes[0]['before_json'] == 'null' and
                json.loads(changes[0]['after_json']) == m._state(o) and
                changes[0]['packet_id'] == provenance['packet_id'] and changes[0]['revision'] == receipt['revision'] and
                receipt['packet_id'] == p['packet_id'] and receipt['packet_sha256'] == p['canonical_sha256'] and
                receipt['game_id'] == d['game_id'] and receipt['source_units_modified'] is False and
                receipt['existing_links_modified'] == 0 and o['reviewer'] == p['reviewer'] and
                o['reason'] == item['reason'] and provenance['role'] == item['role'],
                'Finite header creation/adoption lineage differs')
    return observed


def repair_emitted_document(document, source, source_end):
    """Patch only the five B12 byte-exact preimages; delegate everything else."""
    if source not in REPAIR_SOURCES:
        # In particular, the inherited 3D9703 preimage is neither extended nor
        # reconstructed here. Any unknown missing dispatch is rejected by B11.
        return b11.repair_emitted_document(document, source, source_end)
    contract = load_contract()
    entry = next(e for e in contract['documents'] if e['source_offset'] == source)
    require(source_end == entry['source_end'] and m.digest(document) == entry['preimage_sha256'],
            'Finite B12 header payload/source range is not the exact reviewed preimage')
    check = b11.compiled_header(document, source, source_end)
    require(check == entry['header_before'] and check['word_flag'] and not check['inline_dispatch'] and
            check['physical_spans'] > 0 and check['physical_cards'] > 0,
            'Finite B12 header layout differs')
    require(struct.unpack_from('<I', document, 16)[0] == m.kl.fnv(document[64:]),
            'Finite B12 document body checksum differs')
    out = bytearray(document)
    struct.pack_into('<I', out, 60, check['flags'] | 4)
    fixed = bytes(out)
    require(fixed[:60] == document[:60] and fixed[64:] == document[64:] and
            len(fixed) == len(document) and fixed[60] ^ document[60] == 4 and
            fixed[61:64] == document[61:64] and m.digest(fixed) == entry['fixed_sha256'],
            'Finite B12 header repair changed bytes beyond bit4 at offset60')
    after = b11.publishable_header(fixed, source, source_end)
    require(after == entry['header_after'] and after['inline_dispatch'] and
            after['inline_spans'] == check['physical_spans'] and after['inline_cards'] == check['physical_cards'],
            'Finite B12 repair did not activate the exact existing tables')
    fix = {'source_offset': source, 'before_sha256': m.digest(document), 'after_sha256': m.digest(fixed),
           'before_flags': check['flags'], 'after_flags': after['flags'], 'byte_offset': 60, 'changed_bit': 4,
           'body_and_card_identities_unchanged': True, 'finite_adapter': SCHEMA,
           'contract_sha256': CONTRACT_SHA256}
    return fixed, fix


def runtime_document(conn, d, capacity, metadata=None):
    """Use the historical projection first; add only finite dispatch evidence."""
    entry = None
    if d['source_offset'] in REPAIR_SOURCES:
        contract = load_contract()
        entry = next(e for e in contract['documents'] if e['source_offset'] == d['source_offset'])
        verify_evidence(conn, d, entry, contract)
    document, source_meta, detail = m._runtime_document(conn, d, capacity, metadata)
    if entry is not None:
        parsed = m.kl.document(document)
        require(parsed['spans'] == entry['spans'] and parsed['cards'] == entry['cards'] and
                detail == entry['runtime_detail'], 'Emitted finite B12 tables/detail differ from the reviewed preimage')
    document, fix = repair_emitted_document(document, d['source_offset'], d['source_end'])
    if entry is not None:
        fix = dict(fix, source_units_cards_history_verified=True)
    detail = dict(detail)
    detail['v3_header_fix'] = fix
    return document, source_meta, detail
