#!/usr/bin/env python3
"""Contrato finito de la continuación B12, con linaje B11 intacto.

El contrato se sella después de adoptar paquetes explícitos. No permite
exportar otra revisión por cambiar una constante de un exportador histórico.
"""
from pathlib import Path
import argparse, hashlib, json, re, sqlite3, sys

HERE = Path(__file__).resolve().parent
MEM = HERE.parent
sys.path.insert(0, str(MEM))
import memory as m
from continuation_b11 import export_runtime_v3 as b11

IMMUTABLE = ('entries', 'forms', 'metadata',
             'koto_documents', 'koto_units', 'koto_teacher_notes',
             'koto_document_cards', 'koto_sources', 'koto_grammar_patterns',
             'koto_kanji', 'koto_kanji_navigation', 'koto_kanji_pages')
APPEND_ONLY = ('koto_card_variants', 'koto_lexemes', 'koto_senses',
               'koto_runtime_cards', 'koto_imports', 'koto_packets',
               'koto_changes', 'koto_new_identities', 'koto_document_translations')


def binding_path(item):
    roots = {'memory': MEM, 'project': MEM.parents[2]}
    m.require(item['root'] in roots, 'Raíz de dependencia fuera del contrato')
    relative = Path(item['path'])
    m.require(not relative.is_absolute() and '..' not in relative.parts, 'Ruta de dependencia no portable')
    root = roots[item['root']].resolve(); path = (root / relative).resolve()
    m.require(path.is_relative_to(root) and path.is_file(), 'Dependencia fuera de su raíz o ausente')
    return path


def tables(c):
    # The complete memory also owns the imported dictionary's entries, forms
    # and metadata. A koto_% filter silently excluded their contents from both
    # the finite contract and interrupted-import replay verification.
    return [r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT GLOB 'sqlite_*' ORDER BY name COLLATE BINARY")]


def ordered_rows(c, table):
    m.require(table in tables(c), 'Nombre de tabla ajeno')
    quote = lambda name: '"' + name.replace('"', '""') + '"'
    quoted_table = quote(table)
    cols = list(c.execute(f'PRAGMA table_info({quoted_table})'))
    m.require(cols, 'Tabla sin columnas declaradas')
    pk = [x[1] for x in sorted(cols, key=lambda x: x[5]) if x[5]]
    # forms uses UNIQUE(form, entry_id, kind), not a declared primary key.
    # Ordering all columns also preserves duplicate rows in any other table
    # without a PK. Explicit type and binary ordering make the result depend
    # on values, never rowid, insertion order or a column's text collation.
    # Keep the historical indexed PK traversal for large corpus tables.
    # Only the no-PK fallback needs the complete value/type ordering.
    ordering = ','.join(quote(key) for key in pk) if pk else ','.join(
        part for column in cols for part in
        (f'typeof({quote(column[1])})', f'{quote(column[1])} COLLATE BINARY'))
    return c.execute(f'SELECT * FROM {quoted_table} ORDER BY ' + ordering)


def encoded_row(row):
    # Length-prefixing and explicit type tags prevent ambiguous concatenations.
    out = bytearray()
    for value in row:
        if value is None: tag, data = b'N', b''
        elif isinstance(value, bytes): tag, data = b'B', value
        elif isinstance(value, int): tag, data = b'I', str(value).encode()
        elif isinstance(value, float): tag, data = b'F', value.hex().encode()
        else: tag, data = b'T', str(value).encode()
        out.extend(tag); out.extend(len(data).to_bytes(8, 'little')); out.extend(data)
    return bytes(out)


def table_state(c, table):
    h = hashlib.sha256(); count = 0
    for row in ordered_rows(c, table):
        data = encoded_row(row); h.update(len(data).to_bytes(8, 'little')); h.update(data); count += 1
    return {'rows': count, 'sha256': h.hexdigest()}


def verify_append_only(base, current, table):
    cols = list(base.execute(f'PRAGMA table_info({table})'))
    pk = [(i, x[1]) for i, x in enumerate(cols) if x[5]]
    sql = f'SELECT * FROM {table} WHERE ' + ' AND '.join(name + '=?' for _, name in pk)
    n = 0
    for row in ordered_rows(base, table):
        found = current.execute(sql, [row[i] for i, _ in pk]).fetchone()
        m.require(found is not None and tuple(found) == tuple(row), f'Historia modificada en {table}')
        n += 1
    return n


def imported_record(c, operation):
    pid = operation['packet_id']
    row = c.execute('SELECT * FROM koto_imports WHERE packet_id=?', (pid,)).fetchone()
    saved = c.execute('SELECT packet_json FROM koto_packets WHERE packet_id=?', (pid,)).fetchone()
    m.require(row is not None and saved is not None, 'Falta una operación del libro de adopción')
    packet = json.loads(saved[0]); result = json.loads(row['result_json'])
    m.require(m.digest(packet) == row['packet_sha256'] == operation['packet_sha256'], 'Hash de adopción distinto')
    if 'packet_id' in packet:
        m.require(packet['packet_id'] == pid, 'Identidad de paquete distinta')
    else:
        prefixes = {'kotogba.span_findings/1': 'span_findings:', 'kotogba.variable_links/2': 'variable_links:'}
        m.require(packet['schema'] in prefixes and pid == prefixes[packet['schema']] + m.digest(packet),
                  'Identidad derivada de paquete distinta')
    m.require(result.get('packet_id') == pid, 'Identidad de resultado distinta')
    m.require(m.digest(result) == operation['result_sha256'], 'Resultado de adopción distinto')
    expected_revision = operation.get('revision', operation.get('state_after', {}).get('revision'))
    m.require(type(expected_revision) is int and expected_revision > 39, 'Falta revisión de adopción')
    if 'revision' in result:
        m.require(result['revision'] == expected_revision, 'Revisión registrada distinta')
    revisions = {r[0] for r in c.execute('SELECT DISTINCT revision FROM koto_changes WHERE packet_id=?', (pid,))}
    m.require(not revisions or revisions == {expected_revision}, 'Cambios anotados en otra revisión')
    return {'packet_id': pid, 'packet_sha256': row['packet_sha256'],
            'result_sha256': m.digest(result), 'schema': packet['schema'],
            'revision': expected_revision, 'runtime_revision': result.get('runtime_revision')}


def build(baseline_db, target_db, ledger_path, output):
    ledger = json.loads(Path(ledger_path).read_text())
    m.require(ledger['schema'] == 'kotogba.B12_adoption_ledger/1', 'Libro de adopción incompatible')
    operations = ledger['operations']
    m.require(operations and len({x['packet_id'] for x in operations}) == len(operations), 'Operaciones vacías o duplicadas')
    destinations=[Path(output).resolve(),Path(output).with_name(Path(output).name+'.writing').resolve()]
    protected={Path(baseline_db).resolve(),Path(target_db).resolve(),Path(ledger_path).resolve(),Path(__file__).resolve()}
    protected.update(binding_path({'root':'memory','path':op['packet_file']}) for op in operations)
    protected.update(binding_path({'root':'memory','path':entry} if isinstance(entry,str) else entry) for entry in ledger['bound_files'])
    m.require(len(destinations)==len(set(destinations)) and not(set(destinations)&protected),
              'El contrato o su temporal sobrescribirían una entrada, paquete o dependencia')
    m.require(m.file_sha(baseline_db) == ledger['baseline_sha256'], 'El predecesor no es la B11 exacta del libro')
    previous_state, previous_sha = ledger['baseline_state'], ledger['baseline_sha256']
    for operation in operations:
        m.require(operation['state_before'] == previous_state and operation['database_sha256_before'] == previous_sha,
                  'El libro tiene una discontinuidad de estado o bytes entre adopciones')
        after = operation['state_after']
        m.require(after['game_id'] == previous_state['game_id'] and after['base_sha256'] == previous_state['base_sha256'] and
                  after['revision'] == previous_state['revision'] + 1 and
                  after['runtime_revision'] in (previous_state['runtime_revision'], previous_state['runtime_revision'] + 1),
                  'Transición de revisión inválida en el libro')
        previous_state, previous_sha = after, operation['database_sha256_after']
        packet_file = binding_path({'root':'memory','path':operation['packet_file']})
        m.require(m.file_sha(packet_file)==operation['packet_file_sha256'],
                  'El archivo de un paquete adoptado cambió desde su importación')
        m.require(m.digest(json.loads(packet_file.read_text()))==operation['packet_sha256'],
                  'El archivo de paquete no reproduce el contenido adoptado')
    m.require(m.file_sha(target_db) == previous_sha, 'La memoria final cambió fuera de las adopciones registradas')
    with m.connect(baseline_db, True) as base, m.connect(target_db, True) as current:
        base.execute('PRAGMA temp_store=MEMORY'); current.execute('PRAGMA temp_store=MEMORY')
        base.execute('BEGIN'); current.execute('BEGIN')
        g0 = base.execute('SELECT * FROM koto_games WHERE game_id="bprj1"').fetchone()
        baseline_contract = b11._contract(base, g0)
        g = current.execute('SELECT * FROM koto_games WHERE game_id="bprj1"').fetchone()
        m.require({key: g[key] for key in previous_state} == previous_state, 'El estado final no coincide con el libro')
        m.require(g['revision'] > 39 and g['runtime_revision'] >= 27, 'No es una continuación adoptada de B11')
        for key in g0.keys():
            if key not in ('revision', 'runtime_revision'):
                m.require(g[key] == g0[key], 'La autoridad original del juego cambió: ' + key)
        m.require(m.meta(base, 'database_id') == m.meta(current, 'database_id'), 'Identidad de memoria distinta')
        immutable = {}
        for table in IMMUTABLE:
            old = table_state(base, table)
            m.require(old == table_state(current, table), 'Texto, unidades o fuente histórica cambiados: ' + table)
            immutable[table] = old
        retained = {table: verify_append_only(base, current, table) for table in APPEND_ONLY}
        original_imports = {r[0] for r in base.execute('SELECT packet_id FROM koto_imports')}
        new_imports = {r[0] for r in current.execute('SELECT packet_id FROM koto_imports')} - original_imports
        m.require(new_imports == {x['packet_id'] for x in operations}, 'Hay adopciones fuera del libro explícito')
        adopted = [imported_record(current, op) for op in operations]
        m.require([x['revision'] for x in adopted] == list(range(40, g['revision'] + 1)), 'Secuencia de revisiones incompleta o alterada')
        states = {table: table_state(current, table) for table in tables(current)}
        bindings = []
        for entry in ledger['bound_files']:
            item = {'root': 'memory', 'path': entry} if isinstance(entry, str) else dict(entry)
            m.require(set(item) == {'root', 'path'}, 'Campos de dependencia inválidos')
            path = binding_path(item)
            bindings.append({**item, 'sha256': m.file_sha(path), 'bytes': path.stat().st_size})
        contract = {'schema': 'kotogba.B12_finite_export_contract/1',
                    'database_id': m.meta(current, 'database_id'), 'game_id': 'bprj1',
                    'runtime_base_sha256': g['base_sha256'],
                    'memory_revision': g['revision'], 'runtime_revision': g['runtime_revision'],
                    'baseline_B11': baseline_contract,
                    'baseline_snapshot_sha256': m.file_sha(baseline_db),
                    'original_immutable_tables': immutable, 'historical_rows_preserved': retained,
                    'operations': adopted, 'target_tables': states, 'bound_files': bindings,
                    'adoption_ledger_sha256': m.file_sha(ledger_path),
                    'status': 'DEVELOPMENT_NOT_INSTALLABLE',
                    'scope': 'Estado exacto de memoria y proyección; el cierre lingüístico y las pruebas del ejecutable tienen actas separadas.'}
    m.write_json(output, contract)
    return {'path': str(output), 'sha256': m.file_sha(output), 'memory_revision': contract['memory_revision'],
            'runtime_revision': contract['runtime_revision'], 'operations': len(adopted)}


def verify(c, g, contract_path, expected_file_sha256):
    m.require(re.fullmatch('[0-9a-f]{64}', expected_file_sha256) is not None, 'Exportador no sellado')
    m.require(m.file_sha(contract_path) == expected_file_sha256, 'Contrato finito de exportación alterado')
    contract = json.loads(Path(contract_path).read_text())
    m.require(contract['schema'] == 'kotogba.B12_finite_export_contract/1', 'Esquema de contrato distinto')
    m.require((g['revision'], g['runtime_revision']) == (contract['memory_revision'], contract['runtime_revision']), 'La memoria no es la revisión B12 sellada')
    m.require(g['game_id'] == contract['game_id'] and g['base_sha256'] == contract['runtime_base_sha256'], 'Juego o base distinto')
    m.require(m.meta(c, 'database_id') == contract['database_id'], 'Identidad de memoria distinta')
    for item in contract['bound_files']:
        path = binding_path(item)
        m.require(m.file_sha(path) == item['sha256'], 'Fuente de exportación alterada: ' + item['path'])
    m.require(tables(c) == sorted(contract['target_tables']), 'Inventario de tablas distinto')
    for table, state in contract['target_tables'].items():
        m.require(table_state(c, table) == state, 'Estado de memoria distinto del contrato: ' + table)
    for op in contract['operations']: imported_record(c, op)
    return contract


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline-db', type=Path, required=True); p.add_argument('--db', type=Path, required=True)
    p.add_argument('--ledger', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    print(json.dumps(build(a.baseline_db, a.db, a.ledger, a.output), ensure_ascii=False))
