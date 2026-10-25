#!/usr/bin/env python3
"""Memoria lingüística local de kotoGBA; reutiliza DI03_ES y el códec KL29.

No ejecuta SQLite en la consola. No abre ni importa colecciones personales.
Las correcciones de una aparición nunca reescriben una ficha histórica.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import copy
import fcntl
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import struct
import sys
import tempfile
import unicodedata
import zipfile
import runtime_cards
import document_translations
import kc15_companion

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[2]
sys.path.insert(0, str(PROJECT / 'desarrollo/pruebas_R38/vocab'))
import package as kl
sys.path.insert(0, str(PROJECT / 'desarrollo/pruebas_R39/vocab'))
from build_r39 import compact

_DICTIONARY_MODULE = None


def dictionary_module():
    global _DICTIONARY_MODULE
    if _DICTIONARY_MODULE is None:
        path = PROJECT.parent / 'dictionary_data/DI03_ES/dictionary.py'
        spec = importlib.util.spec_from_file_location('kotogba_di03_authority', path)
        _DICTIONARY_MODULE = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_DICTIONARY_MODULE)
    return _DICTIONARY_MODULE

SCHEMA = 'kotogba.memory/1'
REVIEW_SCHEMA = 'kotogba.review/1'
REVIEW_STATES = {'generated', 'reviewed', 'rejected', 'pending'}
CARD_KEYS = {'identity', 'surface', 'reading', 'meaning_es', 'explanation_es', 'type'}
REPLACEMENT_KEYS = CARD_KEYS - {'identity'}
IMMUTABLE_TABLES = ('koto_sources', 'koto_documents', 'koto_document_cards', 'koto_units',
                    'koto_teacher_notes', 'koto_grammar_patterns')


class ValidationError(ValueError):
    """Datos incompatibles, ambiguos o con preimagen distinta."""


def require(test, message):
    if not test:
        raise ValidationError(message)


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'))


def digest(value):
    return hashlib.sha256(value if isinstance(value, bytes) else canonical(value).encode()).hexdigest()


def file_sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.writing')
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def connect(path, readonly=False):
    path = Path(path).resolve()
    require(path.is_file(), f'No existe la base: {path}')
    conn = sqlite3.connect(f'file:{path}?mode={"ro" if readonly else "rw"}', uri=True, timeout=30)
    conn.row_factory = sqlite3.Row
    conn.execute('PRAGMA foreign_keys=ON')
    return conn


def source(conn, source_id, kind, path, extra=None):
    path = Path(path)
    checksum = file_sha(path)
    conn.execute('INSERT INTO koto_sources VALUES(?,?,?,?,?)',
                 (source_id, kind, checksum, path.name, canonical(extra or {})))
    return {'source_id': source_id, 'sha256': checksum}


def meta(conn, key):
    row = conn.execute('SELECT value FROM koto_meta WHERE key=?', (key,)).fetchone()
    require(row is not None, f'Falta metadato {key}')
    return json.loads(row['value'])


def check_schema(conn):
    require(meta(conn, 'schema') == SCHEMA, 'Esquema de memoria incompatible')


def dictionary_digest(conn):
    h = hashlib.sha256()
    for table, sql in [('entries', 'SELECT id,payload FROM entries ORDER BY id'),
                       ('forms', 'SELECT form,entry_id,kind FROM forms ORDER BY form,entry_id,kind'),
                       ('metadata', 'SELECT key,value FROM metadata ORDER BY key')]:
        h.update(table.encode() + b'\0')
        for row in conn.execute(sql):
            h.update(canonical(list(row)).encode() + b'\n')
    return h.hexdigest()


def document_id(game, offset):
    return f'{game}:BPRJ1_{offset:06X}' if game == 'bprj1' else f'{game}:source:{offset:08X}'


def occurrence_id(doc_id, lo, hi):
    return f'{doc_id}:utf8:{lo}:{hi}'


def clean_display(text):
    # Presentation line wrapping must not create a second lexical entry.
    return re.sub(r'\s+', ' ', text.replace('\n', '')).strip()


def register_card(conn, card, provenance):
    require(set(card) == CARD_KEYS, 'Campos de ficha incompatibles')
    require(type(card['identity']) is int and 0 <= card['identity'] <= 0xffffffff,
            'Identidad de ficha fuera de uint32')
    require(type(card['type']) is int and card['type'] in range(5), 'Tipo de ficha inválido')
    for field in REPLACEMENT_KEYS - {'type'}:
        require(isinstance(card[field], str) and '\0' not in card[field], f'Campo {field} inválido')
    payload_hash = digest(card)
    variant = f'card:{card["identity"]:08x}:{payload_hash}'
    kind = 'grammar' if card['type'] in (1, 2) else 'variable' if card['type'] == 4 else 'vocabulary'
    lexical = {'kind': kind, 'lemma': clean_display(card['surface']), 'reading': clean_display(card['reading'])}
    lemma_id = 'lexeme:' + digest(lexical)
    # Contextual explanations remain on the card variant, never on a lemma.
    sense = {'lexeme_id': lemma_id, 'meaning_es': clean_display(card['meaning_es'])}
    sense_id = 'sense:' + digest(sense)
    p = canonical(provenance)
    conn.execute('INSERT OR IGNORE INTO koto_lexemes VALUES(?,?,?,?,?)',
                 (lemma_id, kind, lexical['lemma'], lexical['reading'], p))
    conn.execute('INSERT OR IGNORE INTO koto_senses VALUES(?,?,?,?)',
                 (sense_id, lemma_id, sense['meaning_es'], p))
    conn.execute('INSERT OR IGNORE INTO koto_card_variants VALUES(?,?,?,?,?,?,?)',
                 (variant, card['identity'], payload_hash, lemma_id, sense_id, canonical(card), p))
    return variant


def prior_review_ledger(project, game):
    """Only recover explicit, exact reviewed links; no inference from ID ranges."""
    reviewed = {}
    paths = [project / 'desarrollo/pruebas_R39/vocab/candidate/occurrence_review_R39.json',
             project / 'desarrollo/pruebas_HITO_A/corpus/candidate_final3/occurrence_review_HITO_A.json']
    source_records = []
    for path in paths:
        if not path.is_file():
            continue
        label = 'HITO_A' if 'HITO_A' in str(path) else 'R39'
        source_records.append((label, path))
        for item in json.loads(path.read_text(encoding='utf-8')):
            offset = item['source_offset']
            if label == 'R39':
                targets = [(x['start'], x['end'], x['identity']) for x in item['active_cards']]
            else:
                targets = [(x['display_byte_start'], x['display_byte_end'], x['card']['identity'])
                           for x in item['after']]
            for lo, hi, identity in targets:
                key = (occurrence_id(document_id(game, offset), lo, hi), identity)
                reviewed[key] = {'stage': label, 'decision_id': item.get('decision_id', item.get('context_id')),
                                 'reason': item.get('reason', 'Revisión contextual heredada explícita')}
    return reviewed, source_records


def init_memory(base, dictionary, output, game='bprj1', flags=None, project=PROJECT):
    base, dictionary, output, project = map(Path, (base, dictionary, output, project))
    require(not output.exists(), 'La inicialización requiere un destino nuevo; no reemplaza una base')
    require(base.is_file() and dictionary.is_file(), 'Faltan el KL29 o el diccionario heredado')
    raw = base.read_bytes()
    header, entries = kl.package(raw)
    require(header[1] == 1, 'Versión KL29 incompatible')
    provenance = json.loads(raw[header[13] * 512:header[13] * 512 + header[14]])
    source_metas = {int(x['id'].split('_')[-1], 16): x for x in provenance['sources']}
    reviewed, ledgers = prior_review_ledger(project, game)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.with_name(output.name + '.initializing')
    require(not staging.exists(), 'Hay una inicialización interrumpida; recuperar en otro destino')
    src = sqlite3.connect(f'file:{dictionary.resolve()}?mode=ro', uri=True)
    db = sqlite3.connect(staging)
    db.row_factory = sqlite3.Row
    try:
        src.backup(db)
        src.close()
        require(db.execute("SELECT value FROM metadata WHERE key='schema'").fetchone()[0]
                == '"kotogba.dictionary/1"', 'DI03_ES tiene un esquema incompatible')
        db.executescript((HERE / 'schema.sql').read_text(encoding='utf-8'))
        db.execute('BEGIN IMMEDIATE')
        dictionary_provenance = source(db, 'DI03_ES', 'dictionary', dictionary,
            {'role': 'Autoridad heredada; entries/forms/metadata conservadas sin modificar',
             'automatic_senses_remain_pending': True})
        base_provenance = source(db, 'KL29:' + digest(raw), 'runtime_corpus', base,
                                 {'game_id': game, 'format': 'KOTOL029/1'})
        for label, path in ledgers:
            source(db, 'review:' + label, 'prior_review', path)
        values = {'schema': SCHEMA, 'schema_version': 1,
                  'database_id': 'memory:' + digest({'dictionary': dictionary_provenance['sha256'],
                                                   'game': game, 'base': digest(raw)}),
                  'dictionary_logical_sha256': dictionary_digest(db),
                  'personal_data_policy': 'No se importan ni modifican colecciones personales',
                  'sense_policy': 'La ficha contextual aprobada manda; las propuestas del diccionario no se aplican automáticamente'}
        db.executemany('INSERT INTO koto_meta VALUES(?,?)', [(k, canonical(v)) for k, v in values.items()])
        db.execute('INSERT INTO koto_games VALUES(?,?,?,?,?,?,?,?)',
                   (game, 'BPRJ Rev.1' if game == 'bprj1' else game, digest(raw), raw,
                    header[9], 0, 0, canonical(provenance)))
        for offset, entry in sorted(entries.items()):
            parsed = kl.document(entry['doc'])
            doc_id = document_id(game, offset)
            sm = source_metas.get(offset, {})
            original_kana = kl.text(entry['doc'], sm['kana_offset'], sm['kana_bytes']) if sm.get('kana_bytes') else None
            db.execute('INSERT INTO koto_documents VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
                       (doc_id, game, offset, entry['end'], digest(entry['doc']), entry['doc'],
                        parsed['raw'], original_kana, parsed['japanese'], parsed['spanish'],
                        canonical(sm), canonical(parsed['fragments']), canonical(parsed['words'])))
            variants = {}
            for ordinal, card in enumerate(parsed['cards']):
                variant = register_card(db, card, base_provenance)
                variants[digest(card)] = variant
                db.execute('INSERT INTO koto_document_cards VALUES(?,?,?)', (doc_id, ordinal, variant))
            for ordinal, unit in enumerate(parsed['units']):
                db.execute('INSERT INTO koto_units VALUES(?,?,?,?,?,?,?,?)',
                    (doc_id, ordinal, unit['raw_start'], unit['raw_end'], unit['display_byte_start'],
                     unit['display_byte_end'], unit['display'], unit['reading']))
            for ordinal, span in enumerate(parsed['spans']):
                oid = occurrence_id(doc_id, span['display_byte_start'], span['display_byte_end'])
                prior = reviewed.get((oid, span['card']['identity']))
                origin = dict(base_provenance, inherited_review=prior,
                              runtime_status='adopted', semantic_complete=False)
                variant = variants[digest(span['card'])]
                db.execute('INSERT INTO koto_occurrences VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                    (oid, doc_id, ordinal, span['raw_start'], span['raw_end'], span['display_byte_start'],
                     span['display_byte_end'], span['flags'], variant, variant, 0,
                     'reviewed' if prior else 'generated', prior['stage'] if prior else '',
                     prior['reason'] if prior else 'Contenido heredado; no se acredita revisión individual en Hito B',
                     canonical(origin), 1))
            for ordinal, note in enumerate(parsed['notes']):
                nm = sm.get('notes', [])[ordinal] if ordinal < len(sm.get('notes', [])) else {}
                note_id = nm.get('note_id', f'{doc_id}:note:{ordinal}')
                db.execute('INSERT INTO koto_teacher_notes VALUES(?,?,?,?,?,?,?,?)',
                    (note_id, doc_id, ordinal, nm.get('editorial', {}).get('pattern'), note[1], note[2],
                     canonical(note), canonical(nm)))
        grammar = project.parent / 'gramatica_excel/editorial_final_docente.json'
        if grammar.is_file():
            gp = source(db, 'grammar:teacher', 'grammar_authority', grammar)
            data = json.loads(grammar.read_text(encoding='utf-8'))
            for pattern in data.get('patterns', []):
                db.execute('INSERT INTO koto_grammar_patterns VALUES(?,?,?)',
                           (pattern['id'], canonical(pattern), canonical(gp)))
        if flags:
            add_flags(db, flags, game)
        runtime_cards.create(db)
        runtime_cards.seed_baseline(db)
        db.commit()
        require(db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok', 'SQLite inicial inválido')
        db.close()
        os.replace(staging, output)
    except BaseException:
        db.close()
        src.close()
        # Leave staging as explicit interrupted recovery evidence, never as final.
        raise
    return validate(output, deep=False)


def add_flags(conn, flags_path, game):
    flags_path = Path(flags_path)
    fp = source(conn, 'flags:' + file_sha(flags_path), 'review_candidates', flags_path,
                {'flags_are_not_demonstrated_errors': True})
    flags = json.loads(flags_path.read_text(encoding='utf-8'))
    for code, rows in flags.items():
        for item in rows:
            doc_id = document_id(game, int(item['source'], 16))
            oid = None
            if 'lo' in item and 'hi' in item:
                oid = occurrence_id(doc_id, item['lo'], item['hi'])
                current = conn.execute('SELECT c.card_json FROM koto_occurrences o JOIN koto_card_variants c '
                    'ON c.variant_id=o.variant_id WHERE o.occurrence_id=?', (oid,)).fetchone()
                require(current is not None, f'Candidato sin aparición: {oid}')
                require(json.loads(current['card_json']) == item['card'], f'Candidato de otra versión: {oid}')
            actual_code = 'grammar_694' if code == 'generic_card_candidate' and item['card']['type'] in (1, 2) else code
            rid = 'review_reason:' + digest({'code': actual_code, 'document': doc_id, 'occurrence': oid, 'item': item})
            conn.execute('INSERT INTO koto_review_reasons VALUES(?,?,?,?,?)',
                         (rid, oid, doc_id, actual_code, canonical({'flag': item, 'provenance': fp})))
            if oid and actual_code == 'grammar_694':
                conn.execute("UPDATE koto_occurrences SET status='pending',reviewer='',reason=? WHERE occurrence_id=?",
                    ('Candidato heurístico de A1; necesita disposición contextual, no es un error demostrado', oid))


def _rows(conn, table):
    return conn.execute(f'SELECT COUNT(*) FROM "{table}"').fetchone()[0]


def validate(path, deep=True):
    conn = connect(path, readonly=True)
    try:
        check_schema(conn)
        require(conn.execute('PRAGMA integrity_check').fetchone()[0] == 'ok', 'SQLite corrupto')
        foreign = conn.execute('PRAGMA foreign_key_check').fetchall()
        require(not foreign, 'Referencias ajenas o rotas')
        for game in conn.execute('SELECT * FROM koto_games'):
            require(digest(game['base_payload']) == game['base_sha256'], 'Base KL29 alterada')
        if deep:
            require(dictionary_digest(conn) == meta(conn, 'dictionary_logical_sha256'), 'Diccionario heredado alterado')
        counts = {t: _rows(conn, t) for t in ['entries', 'forms', 'koto_documents', 'koto_lexemes', 'koto_senses',
                  'koto_card_variants', 'koto_document_cards', 'koto_occurrences', 'koto_units',
                  'koto_teacher_notes', 'koto_grammar_patterns', 'koto_kanji', 'koto_imports', 'koto_changes']}
        stats = dict(conn.execute('SELECT status,COUNT(*) FROM koto_occurrences GROUP BY status').fetchall())
        queues = dict(conn.execute('SELECT code,COUNT(*) FROM koto_review_reasons GROUP BY code').fetchall())
        variants = conn.execute('SELECT COUNT(*) FROM (SELECT runtime_identity FROM koto_card_variants '
                                'GROUP BY runtime_identity HAVING COUNT(*)>1)').fetchone()[0]
        if deep:
            for row in conn.execute('SELECT d.*,g.capacity FROM koto_documents d JOIN koto_games g USING(game_id)'):
                require(digest(row['original_bytes']) == row['source_sha256'], 'Documento original alterado')
                parsed = kl.document(row['original_bytes'])
                require(parsed['japanese'] == row['japanese'] and parsed['spanish'] == row['spanish'], 'Texto derivado incoherente')
                require(parsed['raw'] == row['raw_source'], 'Fuente binaria incoherente')
                historical = [json.loads(r[0]) for r in conn.execute('SELECT v.card_json FROM koto_document_cards c '
                    'JOIN koto_card_variants v USING(variant_id) WHERE document_id=? ORDER BY ordinal', (row['document_id'],))]
                require(historical == parsed['cards'], 'Ordinal o identidad histórica modificados')
                if runtime_cards.present(conn):
                    runtime_cards.validate_document(conn,row,parsed)
                current = conn.execute('SELECT * FROM koto_occurrences WHERE document_id=? ORDER BY ordinal',
                                       (row['document_id'],)).fetchall()
                require(len(current) >= len(parsed['spans']), 'Inventario de apariciones incompleto')
                jp = row['japanese'].encode()
                by_id = {r['occurrence_id']:r for r in current}
                for original in parsed['spans']:
                    oid = occurrence_id(row['document_id'], original['display_byte_start'], original['display_byte_end'])
                    require(oid in by_id, 'Aparición histórica borrada')
                    occ = by_id[oid]
                    require((occ['display_start'],occ['display_end']) ==
                            (original['display_byte_start'],original['display_byte_end']),
                            'Identidad de aparición histórica modificada')
                    if not occ['revision']:
                        require((occ['raw_start'],occ['raw_end']) == (original['raw_start'],original['raw_end']),
                                'Límites de fuente modificados sin revisión')
                    require(0 <= occ['display_start'] < occ['display_end'] <= len(jp), 'Límite UTF-8 inválido')
                    jp[:occ['display_start']].decode(); jp[occ['display_start']:occ['display_end']].decode()
                    baseline = conn.execute('SELECT card_json FROM koto_card_variants WHERE variant_id=?',
                                            (occ['baseline_variant_id'],)).fetchone()[0]
                    require(json.loads(baseline) == original['card'], 'La ficha histórica de una aparición se ha alterado')
                active = sorted((o for o in current if o['active']),key=lambda o:(o['display_start'],o['display_end']))
                require(all(a['display_end'] <= b['display_start'] for a,b in zip(active,active[1:])),
                        'Solapamiento de apariciones activas')
                for o in active:
                    require(0 <= o['raw_start'] < o['raw_end'] <= len(parsed['raw']), 'Límite de fuente inválido')
                    jp[:o['display_start']].decode();jp[o['display_start']:o['display_end']].decode()
        translation_report = document_translations.validate(conn, deep=deep)
        return {'schema': SCHEMA, 'passed': True, 'deep': deep, 'database_id': meta(conn, 'database_id'),
                'counts': counts, 'review_statuses': stats, 'review_queues': queues,
                'document_translations': translation_report,
                'historical_identity_payload_conflicts_preserved': variants,
                'personal_collections_opened': False,
                'scope': 'Validez estructural y procedencia; no certifica por sí sola la corrección semántica ni ejecución en DS'}
    finally:
        conn.close()


def spanish_terms(text):
    stop = {'para','como','este','esta','estas','estos','aquí','aqui','frase','forma','con','que','por',
            'del','las','los','una','uno','unos','unas','hay','ser','son','tiene','puede'}
    return {w for w in re.findall(r'[a-záéíóúñü]{3,}', text.lower()) if w not in stop}


def dictionary_candidates(conn, card, context, limit=6):
    """Rank Spanish alternatives, never select or publish a dictionary default."""
    forms = sorted({clean_display(card['surface']), clean_display(card['reading'])} - {''})
    entry_ids = defaultdict(list)
    for form in forms:
        for variant, pos, chain in dictionary_module().variants(form):
            for row in conn.execute('SELECT entry_id FROM forms WHERE form=?', (variant,)):
                entry_ids[row[0]].append((variant,pos,chain))
    terms = spanish_terms(context) | spanish_terms(card['meaning_es'])
    candidates = []
    for eid in sorted(entry_ids):
        record = json.loads(conn.execute('SELECT payload FROM entries WHERE id=?', (eid,)).fetchone()[0])
        for sense in record.get('senses', []):
            matches=[]
            for form,pos,chain in entry_ids[eid]:
                allowed={pos,'v5k-s' if pos=='v5k' else 'v5r-i' if pos=='v5r' else pos}
                if pos and not allowed.intersection(sense['pos']):continue
                matches.append({'matched_form':form,'inflection':chain,'pos_constraint':pos})
            if not matches:continue
            # Original source-language fields remain internal to DI03_ES.
            gloss = sense.get('gloss', {}).get('spa', [])
            kind = 'original_spanish'
            if not gloss:
                gloss = sense.get('gloss_es_auto', [])
                kind = 'automatic_pending_review'
            text = '; '.join(g['text'] for g in gloss if isinstance(g, dict) and g.get('text'))
            if not text:
                continue
            score = len(terms & spanish_terms(text))
            candidates.append({'entry_id': record['id'], 'sense_number': sense['source_number'],
                'meaning_es': text, 'source_status': kind, 'context_overlap': score,
                'matches': matches,
                'status': 'pending', 'restriction_kanji': sense.get('restrict_kanji', []),
                'restriction_reading': sense.get('restrict_reading', []),
                'selection': 'Propuesta para comparar; no sustituye la acepción activa'})
    candidates.sort(key=lambda r:(-r['context_overlap'],r['entry_id'],r['sense_number']))
    return candidates[:limit]


def _occurrence(conn, oid):
    row = conn.execute('SELECT o.*,v.card_json,v.payload_sha256,d.game_id,d.source_offset,d.original_kana,'
        'd.japanese,d.spanish,d.raw_source,d.fragments_json FROM koto_occurrences o '
        'JOIN koto_card_variants v USING(variant_id) JOIN koto_documents d USING(document_id) '
        'WHERE o.occurrence_id=?', (oid,)).fetchone()
    require(row is not None, f'Aparición desconocida: {oid}')
    return effective_document(conn, row)


def effective_document(conn, document):
    return document_translations.effective_document(conn, document)


def export_review(db_path, output, game='bprj1', reason=None, ids=None, limit=None, offset=0):
    conn = connect(db_path, readonly=True)
    try:
        check_schema(conn)
        grow = conn.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
        require(grow is not None, 'Juego desconocido')
        if ids is None:
            sql = 'SELECT DISTINCT o.occurrence_id FROM koto_occurrences o JOIN koto_documents d USING(document_id)'
            params = [game]
            if reason:
                sql += ' JOIN koto_review_reasons r USING(occurrence_id)'
            sql += ' WHERE d.game_id=? AND o.active=1'
            if reason:
                sql += ' AND r.code=?'; params.append(reason)
            sql += ' ORDER BY d.source_offset,o.display_start,o.display_end'
            ids = [r[0] for r in conn.execute(sql, params)]
        require(len(ids) == len(set(ids)), 'Apariciones duplicadas en la selección')
        chosen = ids[offset:None if limit is None else offset + limit]
        items = []
        for oid in chosen:
            row = _occurrence(conn, oid)
            require(row['game_id'] == game and row['active'], 'La aparición no está activa en este juego')
            card = json.loads(row['card_json'])
            ctx = {'document_id':row['document_id'],'source_offset':row['source_offset'],
                   'japanese':row['japanese'],'original_kana':row['original_kana'],'spanish':row['spanish'],
                   'translation_revision':row['translation_revision'],
                   'translation_sha256':row['translation_sha256'],
                   'raw_hex':row['raw_source'].hex(),
                   'display_byte_start':row['display_start'],'display_byte_end':row['display_end'],
                   'raw_start':row['raw_start'],'raw_end':row['raw_end'],
                   'surface':row['japanese'].encode()[row['display_start']:row['display_end']].decode(),
                   'variables':re.findall(r'\{[^{}]*\}',row['japanese'])}
            reasons = [dict(r) for r in conn.execute('SELECT reason_id,code FROM koto_review_reasons '
                                                   'WHERE occurrence_id=? ORDER BY reason_id', (oid,))]
            item = {'occurrence_id':oid,'expected_revision':row['revision'],
                    'expected_card_sha256':row['payload_sha256'],'context':ctx,'current':card,
                    'current_status':row['status'],'reasons':reasons,
                    'proposals':dictionary_candidates(conn,card,row['spanish']),
                    'provenance':json.loads(row['provenance_json'])}
            item['request_sha256'] = digest(item)
            item['review'] = {'status':'pending','decision':None,'reviewer':'','reason':'','replacement':None}
            items.append(item)
        packet = {'schema':REVIEW_SCHEMA,'schema_version':1,'content_language':'es',
                  'base':{'database_id':meta(conn,'database_id'),'game_id':game,
                          'runtime_base_sha256':grow['base_sha256'],'revision':grow['revision']},
                  'items':items}
        packet['packet_id'] = 'review:' + digest(packet)
        write_json(output, packet)
        return {'packet_id':packet['packet_id'],'items':len(items),'path':str(output),
                'selection_reason':reason,'all_generated_proposals_pending':True}
    finally:
        conn.close()


def _new_card(conn, replacement):
    require(set(replacement) == REPLACEMENT_KEYS, 'Campos de corrección inválidos o identidad impuesta')
    card = {}
    for key in ('surface','reading','meaning_es','explanation_es'):
        require(isinstance(replacement[key], str) and '\0' not in replacement[key], f'Campo inválido {key}')
        replacement[key].encode('utf-8')
        card[key] = kl.wrap(replacement[key], key == 'reading')
    require(card['surface'].strip() and card['meaning_es'].strip(), 'La ficha no tiene lema o acepción española')
    require(not re.search(r'[\u3400-\u9fff]',card['surface']) or card['reading'].strip(),
            'Una nueva ficha con kanji necesita lectura documentada')
    require(type(replacement['type']) is int and replacement['type'] in range(5), 'Tipo inválido')
    card['type'] = replacement['type']
    key = digest(card)
    existing = conn.execute('SELECT runtime_identity,payload_json FROM koto_new_identities WHERE payload_sha256=?', (key,)).fetchone()
    if existing:
        require(json.loads(existing['payload_json']) == card, 'Colisión de contenido en registro de identidades')
        return dict(card, identity=existing['runtime_identity'])
    used = {r[0] for r in conn.execute('SELECT DISTINCT runtime_identity FROM koto_card_variants')}
    used.update(r[0] for r in conn.execute('SELECT runtime_identity FROM koto_new_identities'))
    identity = 0x52000000 | (kl.fnv(canonical(card).encode()) & 0xffffff)
    start = identity
    while identity in used:
        identity = 0x52000000 | ((identity + 1) & 0xffffff)
        require(identity != start, 'Espacio de identidades agotado')
    conn.execute('INSERT INTO koto_new_identities VALUES(?,?,?)', (identity, key, canonical(card)))
    return dict(card, identity=identity)


def _backup(conn, path, packet_hash):
    path = Path(path)
    directory = path.with_name(path.name + '.backups')
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / f'before_{packet_hash}.sqlite'
    compressed=destination.with_suffix(destination.suffix+'.gz')
    manifest=compressed.with_suffix(compressed.suffix+'.json')
    if not destination.exists() and compressed.exists():
        require(manifest.exists(),'Backup comprimido sin manifiesto')
        record=json.loads(manifest.read_text())
        require(record['schema']=='kotogba.compressed_backup/1' and
                file_sha(compressed)==record['compressed_sha256'],'Backup comprimido alterado')
        return {'path':str(compressed),'sha256':record['source_sha256'],
                'compressed_sha256':record['compressed_sha256'],'compression':'gzip'}
    if not destination.exists():
        staging = destination.with_suffix('.writing')
        with sqlite3.connect(f'file:{path.resolve()}?mode=ro',uri=True) as reader:
            with sqlite3.connect(staging) as writer:
                reader.backup(writer)
        os.replace(staging,destination)
    return {'path':str(destination),'sha256':file_sha(destination)}


def _existing_import(conn, pid, ph):
    row = conn.execute('SELECT * FROM koto_imports WHERE packet_id=?',(pid,)).fetchone()
    if row:
        require(row['packet_sha256']==ph, 'El identificador de paquete ya se importó con contenido diferente')
        result = json.loads(row['result_json'])
        result['already_applied'] = True
        return result
    return None


def _store_packet(conn, pid, packet):
    conn.execute('CREATE TABLE IF NOT EXISTS koto_packets(packet_id TEXT PRIMARY KEY REFERENCES koto_imports(packet_id), packet_json TEXT NOT NULL)')
    conn.execute('INSERT INTO koto_packets VALUES(?,?)',(pid,canonical(packet)))


def _state(row):
    return {k:row[k] for k in ('occurrence_id','variant_id','revision','status','reviewer','reason','active','flags')}


def _log_change(conn, game, revision, pid, oid, before, after):
    conn.execute('INSERT INTO koto_changes(game_id,revision,packet_id,occurrence_id,before_json,after_json) '
                 'VALUES(?,?,?,?,?,?)',(game,revision,pid,oid,canonical(before),canonical(after)))


def _runtime_document(conn, d, capacity, metadata=None):
    """One path for export and transaction validation, including approved Spanish."""
    old=d['original_bytes'];before=kl.document(old)
    spans=_active_spans(conn,d['document_id']);retained=runtime_cards.cards(conn,d['document_id'])
    require(len(spans)<=512,'Demasiados intervalos táctiles para el formato DS')
    require(all(a['display_byte_end']<=b['display_byte_start'] for a,b in zip(spans,spans[1:])),
            'Intervalos táctiles solapados')
    translation=document_translations.state(conn,d)
    translated=translation['value']!=document_translations.original(d)
    sm=copy.deepcopy(metadata if metadata is not None else json.loads(d['metadata_json']))
    detail={'compacted':False,'active_spans':len(spans),
        'historical_cards_retained':len(before['cards']),'stable_catalog_cards':len(retained),
        'translation_revision':translation['revision'],'translation_sha256':translation['sha256'],
        'translation_changed':translated}
    if spans==before['spans'] and retained==before['cards']:
        if not translated:
            require(len(old)<=capacity,'Un documento original excede la capacidad del ejecutable')
            return old,sm,detail
        new,delta=old,0
    else:
        new,delta=runtime_cards.replace_inline(old,spans,retained)
    if sm:
        for key in ('id_offset','kana_offset'):
            if sm.get(key):sm[key]+=delta
        for note in sm.get('notes',[]):
            if note.get('original_body_offset'):note['original_body_offset']+=delta
    if translated:new=document_translations.replace_document(new,translation['value'])
    before_compact=len(new)
    if len(new)>capacity:new=compact(new,sm)
    require(len(new)<=capacity,f"Fuente {d['source_offset']:06X}: {len(new)} bytes superan buffer DS de {capacity}; no se recorta contenido")
    parsed=kl.document(new)
    require(parsed['cards'][:len(before['cards'])]==before['cards'],'Se perdieron fichas u ordinales históricos')
    require(parsed['cards']==retained,'No se exportaron todas las fichas del catálogo estable')
    require(parsed['spans']==spans,'Los intervalos exportados no coinciden con la memoria')
    for key in ('raw','raw_hash','units','notes','words','japanese'):
        require(parsed[key]==before[key],f'Exportación alteró {key} sin autorización')
    require(parsed['spanish']==translation['value']['spanish'] and
            parsed['fragments']==[tuple(x) for x in translation['value']['fragments']],
            'La traducción exportada no coincide con la revisión efectiva')
    detail['compacted']=before_compact!=len(new)
    return new,sm,detail


def _validate_changed_documents(conn, game, document_ids):
    """Refuse a transaction that cannot be exported within the NDS ABI."""
    capacity=conn.execute('SELECT capacity FROM koto_games WHERE game_id=?',(game,)).fetchone()[0]
    for doc_id in sorted(set(document_ids)):
        d=conn.execute('SELECT * FROM koto_documents WHERE document_id=?',(doc_id,)).fetchone()
        runtime_cards.sync_document(conn,doc_id)
        _runtime_document(conn,d,capacity)


def import_review(db_path, packet_path, fault_hook=None):
    packet = json.loads(Path(packet_path).read_text(encoding='utf-8'))
    require(set(packet)=={'schema','schema_version','content_language','base','items','packet_id'},'Cabecera de revisión inválida')
    require(packet['schema']==REVIEW_SCHEMA and packet['schema_version']==1,'Esquema de revisión incompatible')
    require(packet['content_language']=='es','El contenido visible debe estar en castellano')
    require(isinstance(packet['packet_id'],str) and packet['packet_id'],'Paquete sin identidad')
    require(isinstance(packet['items'],list) and 0<len(packet['items'])<=10000,'Tamaño de revisión inválido')
    ph,pid = digest(packet),packet['packet_id']
    db_path = Path(db_path)
    with db_path.with_name(db_path.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        conn=connect(db_path)
        try:
            check_schema(conn)
            conn.execute('BEGIN IMMEDIATE')
            prior=_existing_import(conn,pid,ph)
            if prior:conn.rollback();return prior
            base=packet['base']; game=base['game_id']
            require(set(base)=={'database_id','game_id','runtime_base_sha256','revision'} and
                    type(base['revision']) is int and base['revision']>=0,'Identidad o versión base inválida')
            require(base['database_id']==meta(conn,'database_id'),'Revisión de otra memoria')
            grow=conn.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
            require(grow and grow['base_sha256']==base['runtime_base_sha256'],'Revisión de otra base del juego')
            prepared=[];seen=set()
            for item in packet['items']:
                require(set(item)=={'occurrence_id','expected_revision','expected_card_sha256','context','current',
                    'current_status','reasons','proposals','provenance','request_sha256','review'},'Campos de aparición inesperados')
                oid=item['occurrence_id'];require(oid not in seen,'Aparición repetida en el paquete');seen.add(oid)
                immutable={k:v for k,v in item.items() if k not in ('request_sha256','review')}
                require(digest(immutable)==item['request_sha256'],'Se alteró el contexto del paquete de trabajo')
                row=_occurrence(conn,oid)
                require(type(item['expected_revision']) is int and item['expected_revision']>=0,'Versión de aparición inválida')
                require(row['active'] and row['game_id']==game,'Aparición retirada o de otro juego')
                require(row['revision']==item['expected_revision'] and row['payload_sha256']==item['expected_card_sha256'],
                        f'Conflicto de versión: {oid}')
                require(json.loads(row['card_json'])==item['current'],'La preimagen de ficha ha cambiado')
                ctx=item['context']
                require(ctx['japanese']==row['japanese'] and ctx['spanish']==row['spanish'] and
                        ctx.get('translation_revision',0)==row['translation_revision'] and
                        ctx.get('translation_sha256',row['translation_sha256'])==row['translation_sha256'] and
                        ctx['document_id']==row['document_id'] and ctx['raw_hex']==row['raw_source'].hex() and
                        ctx['source_offset']==row['source_offset'] and ctx['original_kana']==row['original_kana'] and
                        (ctx['raw_start'],ctx['raw_end'],ctx['display_byte_start'],ctx['display_byte_end'])==
                        (row['raw_start'],row['raw_end'],row['display_start'],row['display_end']),
                        'El contexto no coincide con la autoridad')
                review=item['review']
                require(set(review)=={'status','decision','reviewer','reason','replacement'},'Campos de revisión inválidos')
                require(review['status'] in REVIEW_STATES,'Estado de revisión inválido')
                require(review['decision'] in ('keep','replace','reject'),'Decisión de revisión pendiente')
                require(isinstance(review['reviewer'],str) and review['reviewer'].strip() and
                        isinstance(review['reason'],str) and review['reason'].strip(),'Falta autor o justificación')
                require(review['status']==('rejected' if review['decision']=='reject' else 'reviewed'),
                        'Una propuesta generada o pendiente no puede publicarse')
                require((review['replacement'] is not None)==(review['decision']=='replace'),'Sustitución incoherente')
                if review['decision']=='replace':
                    require(set(review['replacement'])==REPLACEMENT_KEYS,'La corrección impone campos no permitidos')
                    require(json.loads(row['card_json'])['type']!=4 and review['replacement']['type']!=4,
                            'La revisión léxica no puede reasignar variables del jugador')
                prepared.append((row,review))
            backup=_backup(conn,db_path,ph)
            revision=grow['revision']+1;changed=0;runtime_overrides=0
            result={'schema':'kotogba.review_import/1','packet_id':pid,'packet_sha256':ph,'game_id':game,
                    'revision':revision,'items':len(prepared),'backup':backup,'already_applied':False}
            conn.execute('INSERT INTO koto_imports VALUES(?,?,?,?)',(pid,ph,game,canonical(result)))
            _store_packet(conn,pid,packet)
            for n,(row,review) in enumerate(prepared,1):
                variant=row['variant_id']
                flags=row['flags']
                if review['decision']=='replace':
                    card=_new_card(conn,review['replacement'])
                    variant=register_card(conn,card,{'packet_id':pid,'reviewer':review['reviewer'],'reason':review['reason']})
                    changed+=variant!=row['variant_id']
                if review['decision'] in ('keep','replace'):
                    # Approval binds the exact reviewed contextual card, even
                    # when its existing payload is retained. Otherwise KS5 may
                    # open a broader historical sense that was never approved.
                    # Preserve identity, raw coordinates and all other flags.
                    flags |= 2
                    runtime_overrides+=flags!=row['flags']
                conn.execute('UPDATE koto_occurrences SET variant_id=?,flags=?,revision=revision+1,status=?,reviewer=?,reason=? '
                             'WHERE occurrence_id=?',(variant,flags,review['status'],review['reviewer'],review['reason'],row['occurrence_id']))
                _log_change(conn,game,revision,pid,row['occurrence_id'],_state(row),_state(_occurrence(conn,row['occurrence_id'])))
                if fault_hook:fault_hook('after_item',n)
            conn.execute('UPDATE koto_games SET revision=?,runtime_revision=runtime_revision+? WHERE game_id=?',
                         (revision,int(changed>0 or runtime_overrides>0),game))
            _validate_changed_documents(conn,game,[row['document_id'] for row,_ in prepared])
            result['changed_cards']=changed
            result['runtime_overrides']=runtime_overrides
            conn.execute('UPDATE koto_imports SET result_json=? WHERE packet_id=?',(canonical(result),pid))
            if fault_hook:fault_hook('before_commit',len(prepared))
            conn.commit()
            return result
        except BaseException:
            conn.rollback();raise
        finally:
            conn.close()


def _active_spans(conn, doc_id):
    rows=conn.execute('SELECT o.*,v.card_json FROM koto_occurrences o JOIN koto_card_variants v USING(variant_id) '
        'WHERE document_id=? AND active=1 ORDER BY display_start,display_end',(doc_id,)).fetchall()
    return [dict(raw_start=r['raw_start'],raw_end=r['raw_end'],display_byte_start=r['display_start'],
                 display_byte_end=r['display_end'],flags=r['flags'],card=json.loads(r['card_json'])) for r in rows]


def apply_patches(db_path, patch_path, game='bprj1', reviewer='Damián', fault_hook=None):
    """Import exact Hito A style interval decisions, preserving retired links."""
    packet=json.loads(Path(patch_path).read_text(encoding='utf-8'))
    require(isinstance(packet,dict) and isinstance(packet.get('patches'),list) and packet['patches'],'No hay correcciones')
    require(isinstance(packet.get('base_sha256'),str),'Falta identidad de la base')
    require(reviewer.strip(),'Falta responsable de la revisión')
    ph=digest(packet);pid=packet.get('packet_id','patches:'+ph)
    db_path=Path(db_path)
    with db_path.with_name(db_path.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        conn=connect(db_path)
        try:
            check_schema(conn);conn.execute('BEGIN IMMEDIATE')
            prior=_existing_import(conn,pid,ph)
            if prior:conn.rollback();return prior
            grow=conn.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
            require(grow and grow['base_sha256']==packet['base_sha256'],'Las decisiones apuntan a otra base KL29')
            if 'expected_memory_runtime_revision' in packet:
                expected=packet['expected_memory_runtime_revision']
                require(type(expected)is int and expected==grow['runtime_revision'],
                        'La revisión de contenido del paquete es obsoleta; verificar sus preimágenes antes de volver a prepararlo')
            prepared=[];intervals=defaultdict(list)
            for patch in packet['patches']:
                require(all(k in patch for k in ('source_offset','start','end','visible','before','replacements')),
                        'Corrección incompleta')
                src=patch['source_offset'];doc_id=document_id(game,src)
                d=conn.execute('SELECT * FROM koto_documents WHERE document_id=?',(doc_id,)).fetchone()
                require(d is not None,f'Fuente desconocida {src:06X}')
                lo,hi=patch['start'],patch['end'];jp=d['japanese'].encode()
                require(type(lo)is int and type(hi)is int and 0<=lo<hi<=len(jp),'Intervalo de pantalla inválido')
                require(jp[lo:hi].decode()==patch['visible'],'Superficie de corrección distinta')
                before=[s for s in _active_spans(conn,doc_id) if s['display_byte_start']<hi and lo<s['display_byte_end']]
                require(before==patch['before'] and before,'Preimagen de segmentación distinta')
                require(all(lo<=s['display_byte_start']<s['display_byte_end']<=hi for s in before),
                        'Una corrección corta una aparición anterior sin revisar el resto')
                require(not any(s['card']['type']==4 for s in before),'No se reescriben las variables del jugador')
                require(isinstance(patch['replacements'],list) and patch['replacements'],'No se permite borrar análisis sin reemplazo')
                after=[]
                units=[dict(u) for u in conn.execute('SELECT * FROM koto_units WHERE document_id=? ORDER BY ordinal',(doc_id,))]
                for replacement in patch['replacements']:
                    rlo,rhi=replacement['start'],replacement['end']
                    require(type(rlo)is int and type(rhi)is int and lo<=rlo<rhi<=hi,'Reemplazo fuera del intervalo')
                    require(jp[rlo:rhi].decode()==replacement['visible'],'Superficie de reemplazo distinta')
                    u=[u for u in units if u['display_start']<rhi and rlo<u['display_end']]
                    require(u,'Reemplazo sin correspondencia con fuente')
                    rc={k:replacement['lemma' if k=='surface' else k] for k in REPLACEMENT_KEYS}
                    require(rc['type']!=4,'No se convierte léxico en nombre personal')
                    after.append((rlo,rhi,min(unit['raw_start'] for unit in u),max(unit['raw_end'] for unit in u),rc))
                after.sort()
                require(all(a[1]<=b[0] for a,b in zip(after,after[1:])),'Reemplazos solapados')
                # Every previously linked letter/digit must remain accessible.
                at=0
                for char in d['japanese']:
                    end=at+len(char.encode())
                    if lo<=at and end<=hi and unicodedata.category(char)[0] in 'LN':
                        covered_before=any(s['display_byte_start']<=at<end<=s['display_byte_end'] for s in before)
                        if covered_before:require(sum(a<=at<end<=b for a,b,*_ in after)==1,'La corrección deja glifos sin acceso')
                    at=end
                for a,b in intervals[doc_id]:require(hi<=a or b<=lo,'Correcciones solapadas en el mismo paquete')
                intervals[doc_id].append((lo,hi))
                prepared.append((d,patch,before,after))
            backup=_backup(conn,db_path,ph);revision=grow['revision']+1
            result={'schema':'kotogba.patch_import/1','packet_id':pid,'packet_sha256':ph,'game_id':game,
                    'revision':revision,'patches':len(prepared),'backup':backup,'already_applied':False}
            conn.execute('INSERT INTO koto_imports VALUES(?,?,?,?)',(pid,ph,game,canonical(result)))
            _store_packet(conn,pid,packet)
            for n,(d,patch,before,after) in enumerate(prepared,1):
                doc_id=d['document_id'];changed_before={}
                for old in before:
                    oid=occurrence_id(doc_id,old['display_byte_start'],old['display_byte_end'])
                    row=_occurrence(conn,oid);changed_before[oid]=_state(row)
                    conn.execute('UPDATE koto_occurrences SET active=0,revision=revision+1 WHERE occurrence_id=?',(oid,))
                for lo,hi,rawlo,rawhi,replacement in after:
                    card=_new_card(conn,replacement)
                    provenance={'packet_id':pid,'reviewer':reviewer,'decision_id':patch.get('decision_id'),
                                'reason':patch.get('reason','Corrección contextual explícita'),'source_offset':d['source_offset']}
                    variant=register_card(conn,card,provenance);oid=occurrence_id(doc_id,lo,hi)
                    row=conn.execute('SELECT * FROM koto_occurrences WHERE occurrence_id=?',(oid,)).fetchone()
                    if row:
                        changed_before.setdefault(oid,_state(row))
                        conn.execute("UPDATE koto_occurrences SET variant_id=?,raw_start=?,raw_end=?,flags=2,"
                            "revision=revision+1,status='reviewed',reviewer=?,reason=?,active=1 WHERE occurrence_id=?",
                            (variant,rawlo,rawhi,reviewer,provenance['reason'],oid))
                    else:
                        ordinal=conn.execute('SELECT COALESCE(MAX(ordinal),-1)+1 FROM koto_occurrences WHERE document_id=?',(doc_id,)).fetchone()[0]
                        changed_before[oid]=None
                        conn.execute('INSERT INTO koto_occurrences VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                            (oid,doc_id,ordinal,rawlo,rawhi,lo,hi,2,variant,variant,1,'reviewed',reviewer,
                             provenance['reason'],canonical(provenance),1))
                for oid,old_state in changed_before.items():
                    _log_change(conn,game,revision,pid,oid,old_state,_state(_occurrence(conn,oid)))
                if fault_hook:fault_hook('after_item',n)
            conn.execute('UPDATE koto_games SET revision=?,runtime_revision=runtime_revision+1 WHERE game_id=?',(revision,game))
            _validate_changed_documents(conn,game,[d['document_id'] for d,_,_,_ in prepared])
            if fault_hook:fault_hook('before_commit',len(prepared))
            conn.commit();return result
        except BaseException:
            conn.rollback();raise
        finally:
            conn.close()


def export_runtime(db_path, output, game='bprj1', capacity=None, base_kc15=None, output_kc15=None, fault_hook=None):
    """Export KL29; pass both KC15 paths to publish a validated SD pair.

    A KL29-only projection with revised Spanish is explicitly incomplete for
    installation: the compiled reader obtains its translation from KC15.
    """
    output=Path(output)
    require(output.resolve()!=Path(db_path).resolve(),'El paquete no puede reemplazar la memoria')
    require((base_kc15 is None)==(output_kc15 is None),'Se requieren ambas rutas KC15 para exportar la pareja')
    companion=None;companion_report=None
    if base_kc15 is not None:
        base_kc15=Path(base_kc15);output_kc15=Path(output_kc15)
        require(output.parent.resolve()==output_kc15.parent.resolve(),'La pareja debe publicarse en una misma carpeta')
        require(len({output.resolve(),output_kc15.resolve(),base_kc15.resolve(),Path(db_path).resolve()})==4,
                'Las salidas no pueden reemplazar la memoria ni el compañero base')
    conn=connect(db_path,readonly=True)
    try:
        check_schema(conn);conn.execute('BEGIN')
        g=conn.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
        require(g is not None,'Juego desconocido')
        base=g['base_payload'];h,entries=kl.package(base);h=list(h)
        require(digest(base)==g['base_sha256'],'Base KL29 alterada')
        if base_kc15 is not None:
            companion,companion_report=kc15_companion.export_bytes(conn,base_kc15,game)
            h[11]=companion_report['output_checksum']
        capacity=g['capacity'] if capacity is None else capacity
        require(type(capacity)is int and 512<=capacity<=65024 and capacity%512==0,'Capacidad de sector inválida')
        provenance=json.loads(base[h[13]*512:h[13]*512+h[14]])
        metas={int(s['id'].split('_')[-1],16):s for s in provenance['sources']}
        changed=[];active_count=0
        for d in conn.execute('SELECT * FROM koto_documents WHERE game_id=? ORDER BY source_offset',(game,)):
            off=d['source_offset'];old=d['original_bytes']
            require(entries[off]['doc']==old and entries[off]['end']==d['source_end'],'Documento fuera de la base declarada')
            new,sm,detail=_runtime_document(conn,d,capacity,metas.get(off))
            active_count+=detail['active_spans']
            if new==old:
                continue
            if off in metas:metas[off].clear();metas[off].update(sm)
            entries[off]['doc']=new
            item={'source_offset':off,'source_id':d['document_id'],'before_sha256':digest(old),
                  'after_sha256':digest(new),'bytes':len(new),
                  **{key:detail[key] for key in ('compacted','active_spans','historical_cards_retained','stable_catalog_cards')}}
            # No new provenance keys on untouched historical exports: B04 must
            # remain byte reproducible when no translation revision is present.
            if detail['translation_revision']:
                item['translation_review']={key:detail[key] for key in
                    ('translation_revision','translation_sha256','translation_changed')}
            changed.append(item)
        if not changed and capacity==g['capacity']:
            if companion_report and h[11]!=kl.H.unpack_from(base)[11]:
                amended=bytearray(base);kl.H.pack_into(amended,0,*h);result=bytes(amended)
            else:result=base
        else:
            h[9]=capacity
            out=bytearray(base[:h[8]*512])
            for i,off in enumerate(sorted(entries)):
                e=entries[off];sector=len(out)//512
                struct.pack_into('<4I',out,h[7]*512+16*i,off,e['end'],sector,len(e['doc']))
                out.extend(kl.padded(e['doc']))
                if off in metas:metas[off].update(doc_sector=sector,doc_bytes=len(e['doc']))
            provenance['HITO_B_memory_export']={'schema':SCHEMA,'base_sha256':g['base_sha256'],
                'content_language':'es','changes':changed,
                'historical_ids_and_ordinals_preserved':True,'personal_collections_opened':False,
                'runtime_card_catalog':'C13 y catálogo persistente que sólo se amplía; las fichas retiradas permanecen disponibles',
                'selection_policy':'La acepción contextual aprobada prevalece sobre las propuestas del diccionario'}
            p=json.dumps(provenance,ensure_ascii=False,separators=(',',':')).encode()
            h[13]=len(out)//512;h[14]=len(p);out.extend(kl.padded(p));h[2]=len(out);kl.H.pack_into(out,0,*h)
            result=bytes(out)
        check_h,check_entries=kl.package(result)
        require(len(check_entries)==len(entries),'Pérdida de documentos en exportación')
        for off,e in check_entries.items():require(e==entries[off],'Relectura binaria distinta')
        if companion_report:
            require(check_h[10]==companion_report['ks5_binding'] and check_h[11]==companion_report['output_checksum'],
                    'KL29 no declara el compañero KC15 exportado')
        output.parent.mkdir(parents=True,exist_ok=True)
        staging=output.with_name(output.name+'.writing');staging.write_bytes(result)
        require(file_sha(staging)==digest(result),'Escritura de paquete incompleta')
        if companion is not None:
            kc_staging=output_kc15.with_name(output_kc15.name+'.writing');kc_staging.write_bytes(companion)
            require(file_sha(kc_staging)==companion_report['output_sha256'],'Escritura KC15 incompleta')
            if fault_hook:fault_hook('pair_staged')
            os.replace(kc_staging,output_kc15)
            if fault_hook:fault_hook('pair_kc15_published')
        os.replace(staging,output)
        if fault_hook:fault_hook('pair_kl29_published')
        report={'schema':'kotogba.memory_runtime_export/1','database_id':meta(conn,'database_id'),
            'game_id':game,'base_sha256':g['base_sha256'],'output_sha256':digest(result),'output_bytes':len(result),
            'documents':len(entries),'active_spans':active_count,'changed_documents':changed,'capacity':capacity,
            'max_document_bytes':max(len(e['doc']) for e in entries.values()),
            'historical_cards_and_ordinals_retained':True,
            'source_text_readings_translations_notes_retained':not any(x.get('translation_review',{}).get('translation_changed') for x in changed),
            'source_text_readings_notes_retained':True,
            'reviewed_translation_documents':sum(bool(x.get('translation_review',{}).get('translation_changed')) for x in changed),
            'byte_identical_to_input':result==base,'personal_collections_opened':False}
        report['companion_export_required']=bool(report['reviewed_translation_documents'])
        report['installable_pair_validated']=companion is not None
        if companion_report:
            report['kc15_companion']=companion_report
            report['pair_commit_file']=output.name+'.pair.json'
            pair=dict(schema='kotogba.memory_runtime_pair/1',passed=True,
                scope='Coherencia binaria de exportación; no sustituye a las pruebas del ejecutable.',
                database_id=meta(conn,'database_id'),game_id=game,
                base_KL29_sha256=g['base_sha256'],base_KC15_sha256=companion_report['base_sha256'],
                kl29={'path':output.name,'bytes':len(result),'sha256':digest(result),'kc15_checksum':check_h[11]},
                kc15={'path':output_kc15.name,'bytes':len(companion),'sha256':companion_report['output_sha256'],
                      'checksum':companion_report['output_checksum']},
                paired_binding_verified=True,
                recovery='Si falta esta acta o algún hash no coincide, repetir la exportación completa. Nunca instalar una pareja parcial.')
            write_json(output_kc15.with_suffix(output_kc15.suffix+'.json'),companion_report)
        write_json(output.with_suffix(output.suffix+'.json'),report)
        if companion_report:
            # Commit marker comes last. A process cut between file renames is
            # detectable by its absence or by the old manifest's exact hashes.
            write_json(output.with_name(report['pair_commit_file']),pair)
        return report
    finally:
        conn.close()


def export_inventory(db_path, output, game='bprj1'):
    """Complete source and occurrence inventory, without forcing a fixed count."""
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    conn=connect(db_path,readonly=True)
    try:
        check_schema(conn);counts=Counter()
        with (output/'documents.jsonl').open('w',encoding='utf-8') as docs, \
             (output/'occurrences.jsonl').open('w',encoding='utf-8') as occurrences:
            for d in conn.execute('SELECT * FROM koto_documents WHERE game_id=? ORDER BY source_offset',(game,)):
                d=effective_document(conn,d)
                units=[dict(u) for u in conn.execute('SELECT ordinal,raw_start,raw_end,display_start,display_end,display,reading '
                                                   'FROM koto_units WHERE document_id=? ORDER BY ordinal',(d['document_id'],))]
                notes=[dict(n) for n in conn.execute('SELECT note_id,ordinal,pattern_id,body_es,anchor_jp '
                                                   'FROM koto_teacher_notes WHERE document_id=? ORDER BY ordinal',(d['document_id'],))]
                record={'document_id':d['document_id'],'source_offset':d['source_offset'],'source_end':d['source_end'],
                        'raw_source_hex':d['raw_source'].hex(),'raw_source_sha256':digest(d['raw_source']),
                        'original_kana':d['original_kana'],'japanese':d['japanese'],'spanish':d['spanish'],
                        'original_spanish':d['original_spanish'],'translation_review':d['translation_review'],
                        'units':units,'notes':notes,'fragments':json.loads(d['fragments_json']),
                        'legacy_word_links':json.loads(d['legacy_words_json']),
                        'variables':re.findall(r'\{[^{}]*\}',d['japanese']),
                        'provenance':json.loads(d['metadata_json'])}
                docs.write(canonical(record)+'\n');counts['documents']+=1;counts['teacher_notes']+=len(notes)
                for row in conn.execute('SELECT o.*,v.runtime_identity,v.card_json,v.lexeme_id,v.sense_id '
                    'FROM koto_occurrences o JOIN koto_card_variants v USING(variant_id) '
                    'WHERE document_id=? ORDER BY ordinal',(d['document_id'],)):
                    rec=dict(row);rec['card']=json.loads(rec.pop('card_json'));rec['provenance']=json.loads(rec.pop('provenance_json'))
                    rec['surface']=d['japanese'].encode()[row['display_start']:row['display_end']].decode()
                    occurrences.write(canonical(rec)+'\n');counts['all_occurrences']+=1;counts['active_occurrences']+=row['active']
        report={'schema':'kotogba.memory_inventory/1','counts':dict(counts),
                'distinct_lexemes':_rows(conn,'koto_lexemes'),'distinct_senses':_rows(conn,'koto_senses'),
                'distinct_card_variants':_rows(conn,'koto_card_variants'),
                'files':{p.name:file_sha(p) for p in (output/'documents.jsonl',output/'occurrences.jsonl')},
                'structural_inventory_is_not_semantic_certification':True}
        write_json(output/'inventory.json',report);return report
    finally:conn.close()


def export_bundle(db_path, output):
    """Portable, deterministic database snapshot with checksummed transport."""
    output=Path(output);require(not output.exists(),'El bundle requiere un nombre nuevo')
    conn=connect(db_path,readonly=True)
    output.parent.mkdir(parents=True,exist_ok=True)
    try:
        check_schema(conn)
        # The SQLite snapshot is built outside the synchronized project tree;
        # only its closed, checksummed transport is published there.
        with tempfile.TemporaryDirectory(prefix='kotogba-memory-') as work:
            snap=Path(work)/'memory.sqlite'
            with sqlite3.connect(snap) as dest:conn.backup(dest)
            manifest={'schema':'kotogba.memory_bundle/1','memory_schema':SCHEMA,
                      'database_id':meta(conn,'database_id'),'files':{'memory.sqlite':{'bytes':snap.stat().st_size,'sha256':file_sha(snap)}}}
            staging=output.with_name(output.name+'.writing')
            require(not staging.exists(),'Existe una exportación de bundle interrumpida')
            with zipfile.ZipFile(staging,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
                for name,payload in [('manifest.json',(canonical(manifest)+'\n').encode()),('memory.sqlite',None)]:
                    info=zipfile.ZipInfo(name,date_time=(1980,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED;info.external_attr=0o600<<16
                    if payload is not None:z.writestr(info,payload)
                    else:
                        with z.open(info,'w') as target,snap.open('rb') as src:shutil.copyfileobj(src,target,1024*1024)
            os.replace(staging,output)
            return {'path':str(output),'sha256':file_sha(output),'manifest':manifest}
    finally:conn.close()


def import_bundle(bundle, output):
    output=Path(output);require(not output.exists(),'La restauración siempre usa una copia nueva')
    output.parent.mkdir(parents=True,exist_ok=True)
    staging=output.with_name(output.name+'.restoring');require(not staging.exists(),'Existe una restauración interrumpida')
    with zipfile.ZipFile(bundle) as z:
        require(z.namelist()==['manifest.json','memory.sqlite'],'Contenido de bundle inesperado')
        manifest=json.loads(z.read('manifest.json'))
        require(manifest['schema']=='kotogba.memory_bundle/1' and manifest['memory_schema']==SCHEMA,'Esquema de bundle incompatible')
        expected=manifest['files']['memory.sqlite']
        require(z.getinfo('memory.sqlite').file_size==expected['bytes'],'Tamaño de transporte distinto')
        with z.open('memory.sqlite') as src,staging.open('wb') as dest:shutil.copyfileobj(src,dest,1024*1024)
    require(file_sha(staging)==expected['sha256'],'SHA-256 del bundle incorrecto')
    report=validate(staging,deep=True)
    require(report['database_id']==manifest['database_id'],'Identidad del bundle distinta')
    os.replace(staging,output);return report


def restore_copy(backup, output):
    backup=Path(backup);output=Path(output)
    require(not output.exists(),'La recuperación no sobrescribe una base existente')
    # Historical reports may keep the uncompressed name. Resolve its verified
    # archive without changing the journal or the historical report.
    if not backup.exists() and backup.with_suffix(backup.suffix+'.gz').exists():
        backup=backup.with_suffix(backup.suffix+'.gz')
    output.parent.mkdir(parents=True,exist_ok=True)
    staging=output.with_name(output.name+'.restoring')
    require(not staging.exists(),'Existe una restauración interrumpida')
    try:
        if backup.suffix=='.gz':
            manifest=backup.with_suffix(backup.suffix+'.json')
            require(manifest.exists(),'Backup comprimido sin manifiesto')
            record=json.loads(manifest.read_text())
            require(record['schema']=='kotogba.compressed_backup/1' and
                    file_sha(backup)==record['compressed_sha256'],'Backup comprimido alterado')
            with gzip.open(backup,'rb') as src,staging.open('wb') as dst:
                shutil.copyfileobj(src,dst,1024*1024)
            expected=record['source_sha256']
            require(staging.stat().st_size==record['source_bytes'],'Tamaño descomprimido distinto')
        else:
            expected=file_sha(backup);shutil.copyfile(backup,staging)
        require(file_sha(staging)==expected,'Copia de recuperación incompleta')
        report=validate(staging,deep=True);os.replace(staging,output)
        return dict(report,restored_copy=str(output),source_sha256=expected,backup_transport=str(backup))
    except BaseException:
        if staging.exists():staging.unlink()
        raise


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('init');p.add_argument('--base',type=Path,required=True);p.add_argument('--dictionary',type=Path,required=True)
    p.add_argument('--db',type=Path,required=True);p.add_argument('--game',default='bprj1');p.add_argument('--flags',type=Path)
    p=sub.add_parser('validate');p.add_argument('--db',type=Path,required=True);p.add_argument('--quick',action='store_true')
    p=sub.add_parser('export-runtime');p.add_argument('--db',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--game',default='bprj1');p.add_argument('--capacity',type=int)
    p.add_argument('--base-kc15',type=Path);p.add_argument('--output-kc15',type=Path)
    p=sub.add_parser('inventory');p.add_argument('--db',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--game',default='bprj1')
    p=sub.add_parser('export-review');p.add_argument('--db',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--game',default='bprj1');p.add_argument('--reason');p.add_argument('--ids',type=Path);p.add_argument('--limit',type=int);p.add_argument('--offset',type=int,default=0)
    p=sub.add_parser('import-review');p.add_argument('--db',type=Path,required=True);p.add_argument('--packet',type=Path,required=True)
    p=sub.add_parser('export-translations');p.add_argument('--db',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--ids',type=Path,required=True);p.add_argument('--game',default='bprj1')
    p=sub.add_parser('prepare-translations');p.add_argument('--db',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--proposal',type=Path,action='append',required=True);p.add_argument('--game',default='bprj1')
    p=sub.add_parser('import-translations');p.add_argument('--db',type=Path,required=True);p.add_argument('--packet',type=Path,required=True)
    p=sub.add_parser('apply-patches');p.add_argument('--db',type=Path,required=True);p.add_argument('--patches',type=Path,required=True)
    p.add_argument('--game',default='bprj1');p.add_argument('--reviewer',default='Damián')
    p=sub.add_parser('export-bundle');p.add_argument('--db',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p=sub.add_parser('import-bundle');p.add_argument('--bundle',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p=sub.add_parser('restore-copy');p.add_argument('--backup',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.command=='init':result=init_memory(args.base,args.dictionary,args.db,args.game,args.flags)
    elif args.command=='validate':result=validate(args.db,deep=not args.quick)
    elif args.command=='export-runtime':result=export_runtime(args.db,args.output,args.game,args.capacity,args.base_kc15,args.output_kc15)
    elif args.command=='inventory':result=export_inventory(args.db,args.output,args.game)
    elif args.command=='export-review':result=export_review(args.db,args.output,args.game,args.reason,
        json.loads(args.ids.read_text()) if args.ids else None,args.limit,args.offset)
    elif args.command=='import-review':result=import_review(args.db,args.packet)
    elif args.command=='export-translations':result=document_translations.export_review(args.db,args.output,json.loads(args.ids.read_text()),args.game)
    elif args.command=='prepare-translations':result=document_translations.prepare_proposals(args.db,args.proposal,args.output,args.game)
    elif args.command=='import-translations':result=document_translations.ingest(args.db,args.packet)
    elif args.command=='apply-patches':result=apply_patches(args.db,args.patches,args.game,args.reviewer)
    elif args.command=='export-bundle':result=export_bundle(args.db,args.output)
    elif args.command=='import-bundle':result=import_bundle(args.bundle,args.output)
    else:result=restore_copy(args.backup,args.output)
    print(json.dumps(result,ensure_ascii=False,indent=2))


if __name__=='__main__':
    try:main()
    except (ValidationError,sqlite3.Error,UnicodeError,ValueError,KeyError,TypeError,struct.error) as error:
        print(json.dumps({'passed':False,'error_type':type(error).__name__,'error':str(error)},ensure_ascii=False),file=sys.stderr)
        raise SystemExit(2)
