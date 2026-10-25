#!/usr/bin/env python3
"""Versioned exact-source unions for explicitly reviewed B12 intervals.

prepare is read-only and binds reviewed context, source, predecessor states,
source units, all neighbouring spans and current global authority. apply is
transactional, append-preserving and idempotent. Existing importers stay intact.
Only unions of whole current occurrences are admitted; no coarse unit mapping,
free-form source rewriting, variable rewriting or implicit split is allowed.
"""
from pathlib import Path
import argparse
import copy
import fcntl
import json
import sys

HERE = Path(__file__).resolve().parent
MEM = HERE.parent
sys.path.insert(0, str(MEM))
import memory as m

SCHEMA = 'kotogba.exact_union_import/3'
TOP_KEYS = {'schema', 'packet_id', 'game_id', 'base_sha256', 'database_id',
            'expected_memory_revision', 'expected_runtime_revision',
            'source_proposals', 'source_proposals_sha256', 'source_proposals_runtime_revision',
            'adapter_sources', 'items'}
ITEM_KEYS = {'proposal', 'source_binding', 'original_units', 'outside_spans_sha256',
             'predecessor_states', 'target_preimage', 'exact_raw_start', 'exact_raw_end'}
CONTEXT_KEYS = {'japanese', 'spanish', 'translation_revision', 'translation_sha256'}


def code_identity():
    return {'adapter': m.file_sha(Path(__file__)), 'memory': m.file_sha(MEM / 'memory.py'),
            'runtime_cards': m.file_sha(MEM / 'runtime_cards.py'),
            'document_translations': m.file_sha(MEM / 'document_translations.py')}


def occurrence_state(row):
    if row is None:
        return None
    return {**m._state(row), **{k: row[k] for k in
            ('document_id', 'ordinal', 'raw_start', 'raw_end', 'display_start', 'display_end', 'baseline_variant_id', 'provenance_json')}}


def source_binding(d):
    return {'document_id': d['document_id'], 'source_offset': d['source_offset'],
            'source_sha256': d['source_sha256'], 'raw_source_sha256': m.digest(d['raw_source']),
            'japanese_sha256': m.digest(d['japanese'].encode()),
            'original_bytes_sha256': m.digest(d['original_bytes'])}


def inspect_union(c, proposal, game):
    """Derive endpoints only from exact whole predecessor occurrences."""
    required = {'source_offset', 'start', 'end', 'visible', 'before', 'replacements', 'reason', 'context'}
    m.require(isinstance(proposal, dict) and required <= set(proposal), 'Incomplete reviewed union')
    m.require(type(proposal['source_offset']) is int and proposal['source_offset'] >= 0, 'Invalid source offset')
    m.require(isinstance(proposal['reason'], str) and proposal['reason'].strip(), 'Union lacks semantic reason')
    did = m.document_id(game, proposal['source_offset'])
    d = c.execute('SELECT * FROM koto_documents WHERE document_id=?', (did,)).fetchone()
    m.require(d is not None, 'Unknown union source')
    ctx = proposal['context']
    tr = m.document_translations.state(c, d)
    m.require(isinstance(ctx, dict) and set(ctx) == CONTEXT_KEYS and
              ctx['japanese'] == d['japanese'] and ctx['spanish'] == tr['value']['spanish'] and
              type(ctx['translation_revision']) is int and ctx['translation_revision'] == tr['revision'] and
              ctx['translation_sha256'] == tr['sha256'], 'Reviewed translation or Japanese context changed')
    lo, hi = proposal['start'], proposal['end']
    jp = d['japanese'].encode()
    m.require(type(lo) is int and type(hi) is int and 0 <= lo < hi <= len(jp), 'Invalid union interval')
    try:
        visible = jp[lo:hi].decode()
    except UnicodeDecodeError as exc:
        raise m.ValidationError('Union cuts a UTF-8 glyph') from exc
    m.require(visible == proposal['visible'], 'Literal union surface changed')
    spans = m._active_spans(c, did)
    before = [s for s in spans if s['display_byte_start'] < hi and lo < s['display_byte_end']]
    outside = [s for s in spans if not (s['display_byte_start'] < hi and lo < s['display_byte_end'])]
    m.require(before == proposal['before'] and len(before) >= 2, 'Whole predecessor preimage changed or is not a union')
    m.require(before[0]['display_byte_start'] == lo and before[-1]['display_byte_end'] == hi and
              all(lo <= s['display_byte_start'] < s['display_byte_end'] <= hi for s in before),
              'Union cuts or extends beyond predecessor endpoints')
    m.require(all(a['display_byte_end'] <= b['display_byte_start'] for a, b in zip(before, before[1:])),
              'Overlapping display predecessors')
    m.require(not any(s['card']['type'] == 4 for s in before), 'Player variables cannot be merged')
    replacements = proposal['replacements']
    m.require(isinstance(replacements, list) and len(replacements) == 1, 'Exactly one whole union replacement required')
    replacement = replacements[0]
    m.require(isinstance(replacement, dict) and
              {'start', 'end', 'visible', 'lemma', 'reading', 'meaning_es', 'explanation_es', 'type'} <= set(replacement),
              'Incomplete union replacement')
    m.require(type(replacement['start']) is int and type(replacement['end']) is int and
              (replacement['start'], replacement['end'], replacement['visible']) == (lo, hi, visible),
              'Replacement does not cover exactly the union')
    m.require(type(replacement['type']) is int and replacement['type'] in (0, 1, 2, 3),
              'Invalid union type or variable conversion')
    for key in ('lemma', 'reading', 'meaning_es', 'explanation_es'):
        m.require(isinstance(replacement[key], str) and '\0' not in replacement[key], 'Invalid card text')
    m.require(replacement['lemma'].strip() and replacement['meaning_es'].strip(), 'Empty union card')
    rawlo, rawhi = before[0]['raw_start'], before[-1]['raw_end']
    m.require(type(rawlo) is int and type(rawhi) is int and 0 <= rawlo < rawhi <= len(d['raw_source']) and
              rawlo == min(s['raw_start'] for s in before) and rawhi == max(s['raw_end'] for s in before) and
              all(rawlo <= s['raw_start'] < s['raw_end'] <= rawhi for s in before), 'Inconsistent raw predecessor endpoints')
    m.require(not any(s['raw_start'] < rawhi and rawlo < s['raw_end'] for s in outside),
              'Union would claim source bytes belonging to a neighbour')
    m.require(not any(marker in d['raw_source'][rawlo:rawhi] for marker in (b'\xfd', b'\xf7')),
              'Union source range includes a dynamic marker')
    for where in (proposal, replacement):
        if 'raw_start' in where or 'raw_end' in where:
            m.require(type(where.get('raw_start')) is int and type(where.get('raw_end')) is int and
                      (where['raw_start'], where['raw_end']) == (rawlo, rawhi), 'Proposed raw endpoints differ')
    units = [dict(u) for u in c.execute('SELECT * FROM koto_units WHERE document_id=? AND display_start<? AND display_end>? ORDER BY ordinal',
                                      (did, hi, lo))]
    m.require(units and units[0]['display_start'] <= lo < units[0]['display_end'] and
              units[-1]['display_start'] < hi <= units[-1]['display_end'] and
              units[0]['raw_start'] <= rawlo < units[0]['raw_end'] and
              units[-1]['raw_start'] < rawhi <= units[-1]['raw_end'], 'Endpoints inconsistent with original source units')
    predecessor_states = [occurrence_state(m._occurrence(c, m.occurrence_id(did, s['display_byte_start'], s['display_byte_end']))) for s in before]
    oid = m.occurrence_id(did, lo, hi)
    target = c.execute('SELECT * FROM koto_occurrences WHERE occurrence_id=?', (oid,)).fetchone()
    m.require(target is None or not target['active'], 'The new union already has an active identity')
    if target:
        m.require(target['document_id'] == did and (target['display_start'], target['display_end']) == (lo, hi),
                  'Historical union identity belongs to another interval')
    return d, before, outside, {'proposal': copy.deepcopy(proposal), 'source_binding': source_binding(d),
                               'original_units': units, 'outside_spans_sha256': m.digest(outside),
                               'predecessor_states': predecessor_states, 'target_preimage': occurrence_state(target),
                               'exact_raw_start': rawlo, 'exact_raw_end': rawhi}


def prepare(db, proposals_path, output, allow_unrelated_rebase=False):
    proposals = json.loads(Path(proposals_path).read_text())
    m.require(isinstance(proposals, dict) and isinstance(proposals.get('patches'), list) and
              0 < len(proposals['patches']) <= 10000, 'No union proposals or invalid count')
    before_hash = m.file_sha(db)
    with m.connect(db, readonly=True) as c:
        c.execute('PRAGMA temp_store=MEMORY')
        c.execute('BEGIN')
        m.check_schema(c)
        game = proposals.get('game_id', 'bprj1')
        g = c.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
        m.require(g is not None and proposals.get('base_sha256') == g['base_sha256'], 'Union proposals target another base')
        original_revision = proposals.get('expected_memory_runtime_revision', g['runtime_revision'])
        m.require(type(original_revision) is int and 0 <= original_revision <= g['runtime_revision'], 'Invalid source proposal revision')
        m.require(original_revision == g['runtime_revision'] or allow_unrelated_rebase,
                  'Proposal authority changed; exact unrelated rebase must be explicitly requested')
        items = []
        intervals = {}
        for proposal in proposals['patches']:
            d, _, _, item = inspect_union(c, proposal, game)
            prior = intervals.setdefault(d['document_id'], [])
            lo, hi = proposal['start'], proposal['end']
            m.require(all(hi <= a or b <= lo for a, b in prior), 'Union proposals overlap')
            prior.append((lo, hi))
            items.append(item)
        packet = {'schema': SCHEMA, 'packet_id': 'exact-unions:B12:' + m.digest(proposals), 'game_id': game,
                  'base_sha256': g['base_sha256'], 'database_id': m.meta(c, 'database_id'),
                  'expected_memory_revision': g['revision'], 'expected_runtime_revision': g['runtime_revision'],
                  'source_proposals': copy.deepcopy(proposals),
                  'source_proposals_sha256': m.digest(proposals), 'source_proposals_runtime_revision': original_revision,
                  'adapter_sources': code_identity(), 'items': items}
        c.rollback()
    m.require(m.file_sha(db) == before_hash, 'Read-only preparation changed the authority')
    m.require(not Path(output).exists(), 'Prepare into a fresh file')
    m.write_json(output, packet)
    return {'schema': SCHEMA, 'status': 'PREPARED_NOT_IMPORTED', 'packet_id': packet['packet_id'],
            'packet_sha256': m.digest(packet), 'items': len(items), 'database_writes': 0,
            'memory_revision': packet['expected_memory_revision'], 'runtime_revision': packet['expected_runtime_revision'],
            'source_revision_rebased_with_exact_preimages': original_revision != packet['expected_runtime_revision'],
            'database_sha256_before': before_hash, 'database_sha256_after': m.file_sha(db)}


def apply(db, packet_path, reviewer='Damián', fault_hook=None):
    packet = json.loads(Path(packet_path).read_text())
    m.require(isinstance(packet, dict) and set(packet) == TOP_KEYS and packet['schema'] == SCHEMA, 'Other exact-union schema')
    m.require(isinstance(reviewer, str) and reviewer.strip(), 'Missing reviewer')
    m.require(isinstance(packet['packet_id'], str) and packet['packet_id'].startswith('exact-unions:B12:'), 'Invalid packet identity')
    m.require(isinstance(packet['items'], list) and 0 < len(packet['items']) <= 10000, 'Invalid union count')
    m.require(isinstance(packet['source_proposals'], dict) and
              m.digest(packet['source_proposals']) == packet['source_proposals_sha256'] and
              packet['packet_id'] == 'exact-unions:B12:' + packet['source_proposals_sha256'] and
              packet['source_proposals'].get('patches') == [item.get('proposal') for item in packet['items'] if isinstance(item, dict)],
              'Reviewed semantic proposal binding changed')
    ph, pid = m.digest(packet), packet['packet_id']
    db = Path(db)
    with db.with_name(db.name + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        c = m.connect(db)
        try:
            c.execute('PRAGMA temp_store=MEMORY')
            m.check_schema(c)
            c.execute('BEGIN IMMEDIATE')
            old = m._existing_import(c, pid, ph)
            if old:
                c.rollback()
                return old
            m.require(packet['adapter_sources'] == code_identity(), 'Import source contract changed')
            m.require(m.meta(c, 'database_id') == packet['database_id'], 'Other memory identity')
            game = packet['game_id']
            g = c.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
            m.require(g is not None and g['base_sha256'] == packet['base_sha256'], 'Other game base')
            m.require(type(packet['expected_memory_revision']) is int and type(packet['expected_runtime_revision']) is int and
                      (g['revision'], g['runtime_revision']) == (packet['expected_memory_revision'], packet['expected_runtime_revision']),
                      'Prepared union authority is stale')
            prepared = []
            intervals = {}
            for item in packet['items']:
                m.require(isinstance(item, dict) and set(item) == ITEM_KEYS, 'Invalid exact-union item')
                d, before, outside, check = inspect_union(c, item['proposal'], game)
                m.require(check == item, 'Exact source, units, neighbour or occurrence preimage changed')
                lo, hi = item['proposal']['start'], item['proposal']['end']
                prior = intervals.setdefault(d['document_id'], [])
                m.require(all(hi <= a or b <= lo for a, b in prior), 'Prepared unions overlap')
                prior.append((lo, hi))
                prepared.append((d, item, before, outside))
            backup = m._backup(c, db, ph)
            revision, runtime_revision = g['revision'] + 1, g['runtime_revision'] + 1
            result = {'schema': 'kotogba.exact_union_result/3', 'packet_id': pid, 'packet_sha256': ph,
                      'game_id': game, 'revision': revision, 'runtime_revision': runtime_revision,
                      'unions': len(prepared), 'retired_occurrences': sum(len(x[2]) for x in prepared),
                      'backup': backup, 'already_applied': False, 'new_targets': []}
            c.execute('INSERT INTO koto_imports VALUES(?,?,?,?)', (pid, ph, game, m.canonical(result)))
            m._store_packet(c, pid, packet)
            # Keep a document-wide outside set: a second union in the same
            # document legitimately changes spans outside the first union.
            document_original = {d['document_id']: dict(d) for d, *_ in prepared}
            document_outside = {}
            for did, ranges in intervals.items():
                document_outside[did] = [s for s in m._active_spans(c, did) if
                                         not any(s['display_byte_start'] < hi and lo < s['display_byte_end'] for lo, hi in ranges)]
            for index, (d, item, before, _) in enumerate(prepared, 1):
                proposal = item['proposal']
                did = d['document_id']
                lo, hi = proposal['start'], proposal['end']
                changed = {}
                for previous in before:
                    oid = m.occurrence_id(did, previous['display_byte_start'], previous['display_byte_end'])
                    row = m._occurrence(c, oid)
                    changed[oid] = m._state(row)
                    c.execute('UPDATE koto_occurrences SET active=0,revision=revision+1 WHERE occurrence_id=?', (oid,))
                replacement = proposal['replacements'][0]
                rc = {key: replacement['lemma' if key == 'surface' else key] for key in m.REPLACEMENT_KEYS}
                card = m._new_card(c, rc)
                provenance = {'packet_id': pid, 'reviewer': reviewer, 'decision_id': proposal.get('decision_id'),
                              'reason': proposal['reason'], 'source_offset': d['source_offset'],
                              'exact_source_mapping': {'raw_start': item['exact_raw_start'], 'raw_end': item['exact_raw_end'],
                                                       'method': 'whole-predecessor-endpoints-bound-to-original-units'},
                              'context_binding': proposal['context']}
                variant = m.register_card(c, card, provenance)
                oid = m.occurrence_id(did, lo, hi)
                row = c.execute('SELECT * FROM koto_occurrences WHERE occurrence_id=?', (oid,)).fetchone()
                if row:
                    changed[oid] = m._state(row)
                    c.execute('UPDATE koto_occurrences SET variant_id=?,raw_start=?,raw_end=?,flags=2,revision=revision+1,'
                              'status="reviewed",reviewer=?,reason=?,provenance_json=?,active=1 WHERE occurrence_id=?',
                              (variant, item['exact_raw_start'], item['exact_raw_end'], reviewer, proposal['reason'], m.canonical(provenance), oid))
                else:
                    changed[oid] = None
                    ordinal = c.execute('SELECT COALESCE(MAX(ordinal),-1)+1 FROM koto_occurrences WHERE document_id=?', (did,)).fetchone()[0]
                    c.execute('INSERT INTO koto_occurrences VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                              (oid, did, ordinal, item['exact_raw_start'], item['exact_raw_end'], lo, hi, 2,
                               variant, variant, 1, 'reviewed', reviewer, proposal['reason'], m.canonical(provenance), 1))
                for changed_id, state in changed.items():
                    m._log_change(c, game, revision, pid, changed_id, state, m._state(m._occurrence(c, changed_id)))
                result['new_targets'].append({'occurrence_id': oid, 'card_identity': card['identity'],
                                              'raw_start': item['exact_raw_start'], 'raw_end': item['exact_raw_end'],
                                              'display_start': lo, 'display_end': hi})
                if fault_hook:
                    fault_hook('after_item', index)
            c.execute('UPDATE koto_games SET revision=?,runtime_revision=? WHERE game_id=?', (revision, runtime_revision, game))
            m._validate_changed_documents(c, game, list(intervals))
            for did, ranges in intervals.items():
                m.require(dict(c.execute('SELECT * FROM koto_documents WHERE document_id=?', (did,)).fetchone()) == document_original[did],
                          'Original document/source changed')
                outside = [s for s in m._active_spans(c, did) if
                           not any(s['display_byte_start'] < hi and lo < s['display_byte_end'] for lo, hi in ranges)]
                m.require(outside == document_outside[did], 'An unrelated neighbour changed')
            for target in result['new_targets']:
                row = m._occurrence(c, target['occurrence_id'])
                m.require(row['active'] == 1 and row['status'] == 'reviewed' and row['flags'] & 2 and
                          all(row[k] == target[k] for k in ('raw_start', 'raw_end', 'display_start', 'display_end')),
                          'Published union endpoints/priority changed')
            c.execute('UPDATE koto_imports SET result_json=? WHERE packet_id=?', (m.canonical(result), pid))
            if fault_hook:
                fault_hook('before_commit', len(prepared))
            c.commit()
            return result
        except BaseException:
            c.rollback()
            raise
        finally:
            c.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    q = sub.add_parser('prepare')
    q.add_argument('--db', type=Path, required=True)
    q.add_argument('--proposals', type=Path, required=True)
    q.add_argument('--output', type=Path, required=True)
    q.add_argument('--allow-unrelated-rebase', action='store_true')
    q.add_argument('--report', type=Path)
    q = sub.add_parser('apply')
    q.add_argument('--db', type=Path, required=True)
    q.add_argument('--packet', type=Path, required=True)
    q.add_argument('--reviewer', default='Damián')
    q.add_argument('--report', type=Path)
    args = p.parse_args()
    result = prepare(args.db, args.proposals, args.output, args.allow_unrelated_rebase) if args.command == 'prepare' else apply(args.db, args.packet, args.reviewer)
    if args.report:
        m.write_json(args.report, result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
