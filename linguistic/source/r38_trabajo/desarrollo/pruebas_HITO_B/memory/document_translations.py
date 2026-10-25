#!/usr/bin/env python3
"""Revisiones castellanas de documentos KL29, sin modificar su fuente histórica.

El último registro de cada documento es la única traducción efectiva. Cada
revisión conserva preimagen, dictamen y evidencia adjunta; la base, el japonés,
las variables y los límites de fragmento permanecen inmutables.
"""
from collections import Counter
from pathlib import Path, PurePosixPath
import fcntl
import json
import re
import struct
import sys

SCHEMA = 'kotogba.document_translation_review/1'
TABLE = 'koto_document_translations'
VALUE_KEYS = {'spanish', 'fragments'}
ITEM_KEYS = {'document_id', 'source_offset', 'source_sha256', 'raw_source_sha256',
             'japanese', 'original_kana', 'expected_translation_revision',
             'current', 'current_sha256', 'request_sha256', 'review'}
MARKER = re.compile(r'\{(?:FD|F7):[0-9A-F]{2}\}')
TOKEN = re.compile(r'\{[^{}\r\n]+\}')


def api():
    # memory.py is also a CLI. Reuse its module rather than loading it twice.
    module = sys.modules.get('memory')
    main = sys.modules.get('__main__')
    if module is None and getattr(main, '__file__', None) == str(Path(__file__).with_name('memory.py')):
        module = main
    if module is None:
        import memory as module
    return module


def present(conn):
    return conn.execute('SELECT 1 FROM sqlite_master WHERE type="table" AND name=?', (TABLE,)).fetchone() is not None


def create(conn):
    # execute(), not executescript(): DDL must stay in the caller's transaction.
    conn.execute('CREATE TABLE IF NOT EXISTS koto_document_translations('
        'document_id TEXT NOT NULL REFERENCES koto_documents(document_id),'
        'revision INTEGER NOT NULL CHECK(revision>0),'
        'packet_id TEXT NOT NULL REFERENCES koto_imports(packet_id),'
        'game_revision INTEGER NOT NULL, before_json TEXT NOT NULL, after_json TEXT NOT NULL,'
        'reviewer TEXT NOT NULL, reason_es TEXT NOT NULL, provenance_json TEXT NOT NULL,'
        'PRIMARY KEY(document_id,revision))')
    for operation in ('UPDATE', 'DELETE'):
        conn.execute(f'CREATE TRIGGER IF NOT EXISTS koto_translation_no_{operation.lower()} '
            f'BEFORE {operation} ON koto_document_translations BEGIN '
            "SELECT RAISE(ABORT,'El historial de traducciones sólo admite nuevas revisiones'); END")


def original(document):
    return {'spanish': document['spanish'], 'fragments': json.loads(document['fragments_json'])}


def state(conn, document):
    m = api()
    row = None
    if present(conn):
        row = conn.execute('SELECT * FROM koto_document_translations WHERE document_id=? '
                           'ORDER BY revision DESC LIMIT 1', (document['document_id'],)).fetchone()
    value = json.loads(row['after_json']) if row else original(document)
    return {'revision': row['revision'] if row else 0, 'value': value,
            'sha256': m.digest(value), 'packet_id': row['packet_id'] if row else None,
            'reviewer': row['reviewer'] if row else None,
            'reason_es': row['reason_es'] if row else None,
            'provenance': json.loads(row['provenance_json']) if row else None}


def effective_document(conn, document):
    """Read model used by context producers; never UPDATE koto_documents."""
    result = dict(document)
    current = state(conn, document)
    result['original_spanish'] = document['spanish']
    result['spanish'] = current['value']['spanish']
    result['fragments_json'] = api().canonical(current['value']['fragments'])
    result['translation_revision'] = current['revision']
    result['translation_sha256'] = current['sha256']
    result['translation_review'] = {k: v for k, v in current.items() if k != 'value'}
    return result


def _text(value, allow_empty=False):
    m = api()
    m.require(isinstance(value, str) and (value.strip() or allow_empty and value == ''), 'Traducción vacía o no textual')
    m.require(all(ord(ch) >= 32 or ch == '\n' for ch in value), 'Control no permitido en el castellano')
    encoded = value.encode('utf-8')
    m.require(len(encoded) <= 65024, 'El texto supera el formato de documento DS')
    # Existing brace tokens include legacy aliases and icon controls as well
    # as FD/F7. Preserve their spelling and multiplicity; never normalize them.
    residue = TOKEN.sub('', value)
    m.require('{' not in residue and '}' not in residue, 'Marcador variable mal formado o desconocido')
    return Counter(TOKEN.findall(value))


def validate_value(document, value):
    m = api()
    m.require(isinstance(value, dict) and set(value) == VALUE_KEYS, 'Campos de traducción incompatibles')
    base = original(document)
    old_markers = Counter(MARKER.findall(base['spanish']))
    m.require(_text(value['spanish'], base['spanish'] == '') == Counter(TOKEN.findall(base['spanish'])),
              'Se eliminó, duplicó o cambió una variable de la traducción')
    # Confirm each retained token also names a real original unit. Reordering
    # a translation is permitted; inventing a token or a personal value is not.
    units = m.kl.document(document['original_bytes'])['units']
    actual = Counter()
    for unit in units:
        markers = MARKER.findall(unit['display'])
        if markers:
            raw = document['raw_source'][unit['raw_start']:unit['raw_end']]
            expected = Counter(bytes(int(part, 16) for part in token[1:-1].split(':')) for token in markers)
            found = Counter(re.findall(rb'[\xfd\xf7][\x00-\xff]', raw))
            m.require(found == expected, 'Variable sin correspondencia binaria exacta')
            actual.update(markers)
    m.require(not (old_markers - actual), 'Traducción base con variables no acreditadas por sus unidades')
    fragments = value['fragments']
    m.require(isinstance(fragments, list) and len(fragments) == len(base['fragments']),
              'La revisión debe conservar y declarar todos los fragmentos')
    for previous, replacement in zip(base['fragments'], fragments):
        m.require(isinstance(replacement, list) and len(replacement) == 3 and
                  all(type(x) is int for x in replacement[:2]) and replacement[:2] == previous[:2],
                  'No se pueden mover límites o identidades de fragmentos')
        m.require(_text(replacement[2], previous[2] == '') == Counter(TOKEN.findall(previous[2])),
                  'La revisión altera las variables de un fragmento')


def validate_evidence(evidence):
    m = api()
    m.require(isinstance(evidence, dict) and set(evidence) == {'description', 'artifact_utf8'},
              'Se requiere procedencia y evidencia portable adjunta')
    description = evidence['description']
    m.require(isinstance(description, dict), 'Procedencia no estructurada')
    for key in ('id', 'author', 'title', 'source_kind', 'finding_es', 'application_es',
                'artifact_path', 'artifact_sha256'):
        m.require(isinstance(description.get(key), str) and description[key].strip(), 'Falta procedencia: ' + key)
    path = PurePosixPath(description['artifact_path'])
    m.require(not path.is_absolute() and '..' not in path.parts, 'Ruta de procedencia no portable')
    m.require(isinstance(evidence['artifact_utf8'], str) and
              len(evidence['artifact_utf8'].encode()) <= 1024 * 1024, 'Evidencia adjunta ausente o excesiva')
    m.require(m.digest(evidence['artifact_utf8'].encode()) == description['artifact_sha256'],
              'Hash de la evidencia adjunta distinto de su procedencia')


def replace_document(old, value):
    """Only append selected Spanish strings and update their existing pointers."""
    m = api()
    before = m.kl.document(old)
    expected_fragments = [tuple(row) for row in value['fragments']]
    if before['spanish'] == value['spanish'] and before['fragments'] == expected_fragments:
        return old
    out = bytearray(old)
    h = list(before['header'])
    def append(text):
        encoded = text.encode('utf-8')
        if not encoded:
            return 0, 0
        at = len(out)
        out.extend(encoded + b'\0')
        return at, len(encoded)
    h[12], h[13] = append(value['spanish'])
    for n, (lo, hi, text) in enumerate(expected_fragments):
        at, size = append(text)
        struct.pack_into('<4I', out, h[10] + 16 * n, lo, hi, at, size)
    h[2] = len(out)
    h[3] = m.kl.fnv(out[64:])
    m.kl.H.pack_into(out, 0, *h)
    result = bytes(out)
    after = m.kl.document(result)
    for key in ('raw', 'raw_hash', 'units', 'notes', 'words', 'cards', 'spans', 'japanese'):
        m.require(before[key] == after[key], 'La traducción modificó ' + key)
    m.require(after['spanish'] == value['spanish'] and after['fragments'] == expected_fragments,
              'La exportación no contiene la traducción aceptada')
    return result


def export_review(db_path, output, ids, game='bprj1'):
    m = api()
    m.require(isinstance(ids, (list, tuple)) and ids and len(ids) == len(set(ids)), 'Selección de documentos inválida')
    c = m.connect(db_path, True)
    try:
        m.check_schema(c); c.execute('BEGIN')
        g = c.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
        m.require(g is not None, 'Juego desconocido')
        items = []
        for did in ids:
            d = c.execute('SELECT * FROM koto_documents WHERE document_id=?', (did,)).fetchone()
            m.require(d is not None and d['game_id'] == game, 'Documento desconocido o de otro juego')
            current = state(c, d)
            item = {key: d[key] for key in ('document_id', 'source_offset', 'source_sha256', 'japanese', 'original_kana')}
            item.update(raw_source_sha256=m.digest(d['raw_source']),
                        expected_translation_revision=current['revision'], current=current['value'],
                        current_sha256=current['sha256'])
            item['request_sha256'] = m.digest(item)
            item['review'] = {'status': 'pending', 'decision': None, 'reviewer': '', 'reason_es': '',
                              'replacement': None, 'evidence': None}
            items.append(item)
        packet = {'schema': SCHEMA, 'schema_version': 1, 'content_language': 'es',
                  'base': {'database_id': m.meta(c, 'database_id'), 'game_id': game,
                           'runtime_base_sha256': g['base_sha256'], 'revision': g['revision']}, 'items': items}
        packet['packet_id'] = 'translation_review:' + m.digest(packet)
        m.write_json(output, packet)
        return {'packet_id': packet['packet_id'], 'items': len(items), 'path': str(output)}
    finally:
        c.close()


def prepare_proposals(db_path, proposal_paths, output, game='bprj1'):
    """Attach exact existing review evidence; do not mutate the database."""
    m = api()
    proposals = [json.loads(Path(path).read_text(encoding='utf-8')) for path in proposal_paths]
    export_review(db_path, output, [p['document_id'] for p in proposals], game)
    packet = json.loads(Path(output).read_text(encoding='utf-8'))
    for item, proposal in zip(packet['items'], proposals):
        m.require(proposal['schema'] == 'kotogba.document_translation_proposal/1' and
                  proposal['status'] == 'individually_reviewed_proposed_not_imported', 'Propuesta de otro esquema o estado')
        for key in ('document_id', 'source_offset', 'source_sha256', 'raw_source_sha256', 'japanese', 'original_kana'):
            m.require(item[key] == proposal[key], 'Preimagen de propuesta diferente: ' + key)
        m.require(item['current']['spanish'] == proposal['before_spanish'], 'El castellano ya cambió desde la propuesta')
        # A sentence-only proposal is insufficient to rewrite paged fragments.
        m.require(not item['current']['fragments'], 'Usar un paquete completo para revisar traducciones con fragmentos')
        description = proposal['evidence']
        path = PurePosixPath(description['artifact_path'])
        m.require(not path.is_absolute() and '..' not in path.parts, 'Ruta de evidencia no portable')
        evidence_path = (m.PROJECT / str(path)).resolve()
        m.require(evidence_path.is_relative_to(m.PROJECT.resolve()), 'Evidencia fuera del checkpoint')
        evidence = {'description': description, 'artifact_utf8': evidence_path.read_bytes().decode('utf-8')}
        validate_evidence(evidence)
        item['review'] = {'status': 'reviewed', 'decision': 'replace', 'reviewer': proposal['reviewer'],
                          'reason_es': proposal['reason_es'],
                          'replacement': {'spanish': proposal['replacement_spanish'], 'fragments': []},
                          'evidence': evidence}
    m.write_json(output, packet)
    return {'packet_id': packet['packet_id'], 'items': len(packet['items']),
            'packet_sha256': m.digest(packet), 'database_changed': False}


def ingest(db_path, packet_path, fault_hook=None):
    m = api()
    packet = json.loads(Path(packet_path).read_text(encoding='utf-8'))
    m.require(isinstance(packet, dict) and set(packet) ==
              {'schema', 'schema_version', 'content_language', 'base', 'items', 'packet_id'}, 'Cabecera de traducción inválida')
    m.require(packet['schema'] == SCHEMA and type(packet['schema_version']) is int and
              packet['schema_version'] == 1 and packet['content_language'] == 'es',
              'Esquema o lengua de traducción incompatibles')
    m.require(isinstance(packet['packet_id'], str) and packet['packet_id'].strip(), 'Paquete sin identidad')
    m.require(isinstance(packet['items'], list) and 0 < len(packet['items']) <= 10000, 'Tamaño de revisión inválido')
    ph, pid = m.digest(packet), packet['packet_id']; db_path = Path(db_path)
    with db_path.with_name(db_path.name + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        c = m.connect(db_path)
        try:
            m.check_schema(c); c.execute('BEGIN IMMEDIATE')
            prior = m._existing_import(c, pid, ph)
            if prior:
                c.rollback(); return prior
            base = packet['base']
            m.require(isinstance(base, dict) and set(base) ==
                      {'database_id', 'game_id', 'runtime_base_sha256', 'revision'} and
                      type(base['revision']) is int and base['revision'] >= 0, 'Identidad base inválida')
            game = base['game_id']; g = c.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
            m.require(g is not None and g['base_sha256'] == base['runtime_base_sha256'] and
                      m.meta(c, 'database_id') == base['database_id'], 'Traducción de otra memoria o juego')
            prepared = []; seen = set()
            for item in packet['items']:
                m.require(isinstance(item, dict) and set(item) == ITEM_KEYS, 'Campos de revisión de documento inesperados')
                did = item['document_id']; m.require(did not in seen, 'Documento duplicado'); seen.add(did)
                immutable = {k: v for k, v in item.items() if k not in ('request_sha256', 'review')}
                m.require(m.digest(immutable) == item['request_sha256'], 'Contexto del paquete alterado')
                d = c.execute('SELECT * FROM koto_documents WHERE document_id=?', (did,)).fetchone()
                m.require(d is not None and d['game_id'] == game, 'Documento desconocido o de otro juego')
                for key in ('source_offset', 'source_sha256', 'japanese', 'original_kana'):
                    m.require(d[key] == item[key], 'La fuente original no coincide: ' + key)
                m.require(m.digest(d['raw_source']) == item['raw_source_sha256'], 'La fuente ROM no coincide')
                current = state(c, d)
                m.require(type(item['expected_translation_revision']) is int and
                          item['expected_translation_revision'] == current['revision'] and
                          item['current_sha256'] == current['sha256'] and item['current'] == current['value'],
                          'Conflicto de preimagen o revisión de traducción')
                review = item['review']
                m.require(isinstance(review, dict) and set(review) ==
                          {'status', 'decision', 'reviewer', 'reason_es', 'replacement', 'evidence'}, 'Campos de dictamen inválidos')
                m.require(review['status'] == 'reviewed' and review['decision'] == 'replace',
                          'Una traducción pendiente, generada o rechazada no puede publicarse')
                for key in ('reviewer', 'reason_es'):
                    m.require(isinstance(review[key], str) and review[key].strip(), 'Falta responsable o razón de traducción')
                validate_value(d, review['replacement']); validate_evidence(review['evidence'])
                prepared.append((d, current, review))
            backup = m._backup(c, db_path, ph)
            revision = g['revision'] + 1
            changed = sum(current['value'] != review['replacement'] for _, current, review in prepared)
            result = {'schema': 'kotogba.document_translation_import/1', 'packet_id': pid, 'packet_sha256': ph,
                      'game_id': game, 'revision': revision, 'items': len(prepared), 'changed_translations': changed,
                      'backup': backup, 'already_applied': False, 'original_documents_changed': 0,
                      'personal_collections_opened': False}
            c.execute('INSERT INTO koto_imports VALUES(?,?,?,?)', (pid, ph, game, m.canonical(result)))
            m._store_packet(c, pid, packet); create(c)
            for n, (d, current, review) in enumerate(prepared, 1):
                c.execute('INSERT INTO koto_document_translations VALUES(?,?,?,?,?,?,?,?,?)',
                          (d['document_id'], current['revision'] + 1, pid, revision,
                           m.canonical(current['value']), m.canonical(review['replacement']),
                           review['reviewer'], review['reason_es'], m.canonical(review['evidence'])))
                if fault_hook: fault_hook('after_item', n)
            c.execute('UPDATE koto_games SET revision=?,runtime_revision=runtime_revision+? WHERE game_id=?',
                      (revision, int(changed > 0), game))
            m._validate_changed_documents(c, game, seen)
            if fault_hook: fault_hook('before_commit', len(prepared))
            c.commit(); return result
        except BaseException:
            c.rollback(); raise
        finally:
            c.close()


def validate(conn, deep=True):
    m = api()
    if not present(conn):
        return {'schema': SCHEMA, 'revisions': 0, 'documents': 0, 'passed': True}
    chains = {}; packets = {}
    for row in conn.execute('SELECT * FROM koto_document_translations ORDER BY document_id,revision'):
        did = row['document_id']; d = conn.execute('SELECT * FROM koto_documents WHERE document_id=?', (did,)).fetchone()
        m.require(d is not None, 'Traducción huérfana')
        number, previous = chains.get(did, (0, original(d)))
        m.require(row['revision'] == number + 1 and json.loads(row['before_json']) == previous,
                  'Historial de traducción incompleto o preimagen alterada')
        value = json.loads(row['after_json']); validate_value(d, value)
        evidence = json.loads(row['provenance_json']); validate_evidence(evidence)
        pid = row['packet_id']
        if pid not in packets:
            p = conn.execute('SELECT p.packet_json,i.packet_sha256,i.game_id FROM koto_packets p '
                             'JOIN koto_imports i USING(packet_id) WHERE packet_id=?', (pid,)).fetchone()
            m.require(p is not None, 'Traducción sin paquete importado')
            payload = json.loads(p['packet_json'])
            m.require(m.digest(payload) == p['packet_sha256'] and payload['schema'] == SCHEMA and
                      p['game_id'] == d['game_id'], 'Paquete de traducción alterado')
            packets[pid] = {item['document_id']: item for item in payload['items']}
        item = packets[pid].get(did)
        m.require(item is not None and item['source_sha256'] == d['source_sha256'] and
                  item['expected_translation_revision'] == number and item['current'] == previous and
                  item['current_sha256'] == m.digest(previous) and
                  item['review'] == {'status': 'reviewed', 'decision': 'replace', 'reviewer': row['reviewer'],
                    'reason_es': row['reason_es'], 'replacement': value, 'evidence': evidence},
                  'La revisión no coincide con su paquete importado')
        chains[did] = (number + 1, value)
    if deep:
        for did in chains:
            d = conn.execute('SELECT d.*,g.capacity FROM koto_documents d JOIN koto_games g USING(game_id) '
                             'WHERE document_id=?', (did,)).fetchone()
            m._runtime_document(conn, d, d['capacity'])
    return {'schema': SCHEMA, 'revisions': sum(v[0] for v in chains.values()),
            'documents': len(chains), 'passed': True, 'original_documents_changed': 0}
