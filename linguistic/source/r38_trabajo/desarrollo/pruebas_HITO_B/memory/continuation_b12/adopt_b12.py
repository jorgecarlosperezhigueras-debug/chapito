#!/usr/bin/env python3
"""Escritor único B12: adopta un paquete explícito y conserva su acta.

Los importadores mantienen sus propias comprobaciones y transacciones. Este
controlador no cambia sus contratos; añade trazabilidad y comprime respaldos
cerrados tras verificar la descompresión.
"""
from pathlib import Path
import argparse, fcntl, gzip, hashlib, json, os, shutil, sys, tempfile
HERE = Path(__file__).resolve().parent
MEM = HERE.parent
sys.path[:0] = [str(HERE), str(MEM)]
import memory as m
import archive_backups
from continuation_b10 import variable_links_v2
import exact_union_v3
import span_findings
import document_translations
import finite_contract_v4 as finite

B11_SHA256 = '059d2fc2bc6e3b95ed34a9da3e58813570cebc7dbfc2322051f1d28838df661a'


def state(db):
    with m.connect(db, True) as c:
        return dict(c.execute('SELECT game_id,base_sha256,revision,runtime_revision FROM koto_games WHERE game_id="bprj1"').fetchone())


def seed_initial_backup(db, packet):
    if m.file_sha(db) != B11_SHA256: return
    gz = MEM / 'data/kotogba_memory_b11.sqlite.gz'
    h = hashlib.sha256(); n = 0
    with gzip.open(gz, 'rb') as stream:
        while block := stream.read(1024 * 1024): h.update(block); n += len(block)
    m.require(h.hexdigest() == B11_SHA256 and n == db.stat().st_size, 'Transporte inicial distinto')
    directory = db.with_name(db.name + '.backups'); directory.mkdir(exist_ok=True)
    target = directory / ('before_' + m.digest(packet) + '.sqlite.gz')
    if not target.exists(): os.link(gz, target)
    m.require(m.file_sha(target) == m.file_sha(gz), 'Respaldo inicial alterado')
    m.write_json(target.with_suffix(target.suffix + '.json'), {
        'schema': 'kotogba.compressed_backup/1', 'source_name': target.stem,
        'source_sha256': B11_SHA256, 'source_bytes': n, 'compressed_sha256': m.file_sha(target),
        'compressed_bytes': target.stat().st_size, 'decompression_sha256_verified': True,
        'sqlite_quick_check': 'ok'})


def prepare_compressed_predecessor(db, packet):
    """Copia cerrada exacta que el importador histórico verifica y reutiliza."""
    db = Path(db)
    directory = db.with_name(db.name + '.backups'); directory.mkdir(exist_ok=True)
    target = directory / ('before_' + m.digest(packet) + '.sqlite.gz')
    manifest = target.with_suffix(target.suffix + '.json')
    with db.with_name(db.name + '.lock').open('a') as lock:
        import fcntl, shutil
        fcntl.flock(lock, fcntl.LOCK_EX)
        m.require(not db.with_name(db.name + '-wal').exists() and not db.with_name(db.name + '-journal').exists(),
                  'Se exige una SQLite cerrada sin WAL ni journal')
        source_sha = m.file_sha(db)
        if target.exists():
            record = json.loads(manifest.read_text())
            m.require(record['source_sha256'] == source_sha and record['compressed_sha256'] == m.file_sha(target),
                      'El respaldo preexistente pertenece a otro estado')
            return target, manifest
        stage = target.with_name(target.name + '.writing')
        with db.open('rb') as src, stage.open('wb') as raw:
            with gzip.GzipFile(filename='', fileobj=raw, mode='wb', compresslevel=6, mtime=0) as out:
                shutil.copyfileobj(src, out, 1024 * 1024)
        h = hashlib.sha256(); n = 0
        with gzip.open(stage, 'rb') as stream:
            while block := stream.read(1024 * 1024): h.update(block); n += len(block)
        m.require(h.hexdigest() == source_sha == m.file_sha(db) and n == db.stat().st_size,
                  'La descompresión del respaldo no reproduce la memoria exacta')
        with m.connect(db, True) as c: m.require(c.execute('PRAGMA quick_check').fetchone()[0] == 'ok', 'Respaldo SQLite inválido')
        os.replace(stage, target)
        m.write_json(manifest, {'schema':'kotogba.compressed_backup/1','source_name':target.stem,
            'source_sha256':source_sha,'source_bytes':n,'compressed_sha256':m.file_sha(target),
            'compressed_bytes':target.stat().st_size,'decompression_sha256_verified':True,'sqlite_quick_check':'ok'})
        return target, manifest


def invoke(db, packet_path, kind):
    """Los contratos de los importadores se ejecutan sin adaptarlos ni omitirlos."""
    if kind == 'review': result = m.import_review(db, packet_path)
    elif kind == 'variables2':
        registry = MEM / 'continuation_b10/assembly/ROLE_REGISTRY_B10_v2.json'
        result = variable_links_v2.ingest(db, packet_path, registry_path=registry, expected_packet_sha256=m.file_sha(packet_path))
    elif kind == 'variables3':
        from variables import variable_links_v3
        registry = HERE / 'variables/ROLE_REGISTRY_B12_v3_APPROVED.json'
        result = variable_links_v3.ingest(db, packet_path, registry_path=registry,
            approved_registry_sha256=m.file_sha(registry), expected_packet_sha256=m.file_sha(packet_path))
    elif kind == 'variable_spanish':
        from variables import variable_card_refinement_b12_v1 as refinement
        result = refinement.ingest(db, packet_path, expected_packet_sha256=m.file_sha(packet_path))
    elif kind == 'raw_coordinates':
        from second_read import finite_raw_refinement_b12_v1 as refinement
        result = refinement.ingest(db, packet_path, expected_packet_sha256=m.file_sha(packet_path))
    elif kind == 'unions3': result = exact_union_v3.apply(db, packet_path, reviewer='Damián — adopción B12 con segunda lectura')
    elif kind == 'findings': result = span_findings.ingest(db, packet_path)
    elif kind == 'translations': result = document_translations.ingest(db, packet_path)
    else: raise ValueError(kind)
    return result


def persist_json(path, value):
    m.write_json(path, value)
    with Path(path).open('rb') as stream: os.fsync(stream.fileno())
    fd = os.open(Path(path).parent, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)


def source_bindings(kind):
    paths = ['memory.py','runtime_cards.py','document_translations.py','kc15_companion.py',
             'archive_backups.py','continuation_b12/adopt_b12.py','continuation_b12/finite_contract_v4.py']
    extra = {'review': [], 'variables2': ['continuation_b10/variable_links_v2.py','variable_links.py'],
             'variables3': ['continuation_b10/variable_links_v2.py','variable_links.py','continuation_b12/variables/variable_links_v3.py'],
             'variable_spanish': ['continuation_b12/variables/variable_card_refinement_b12_v1.py'],
             'raw_coordinates': ['continuation_b12/second_read/finite_raw_refinement_b12_v1.py',
                                 'continuation_b12/second_read/WATAKUSHI_16E64C_RAW_BOUNDARY_PROVEN.json',
                                 'continuation_b12/second_read/KIKA_3BE976_RAW_BOUNDARY_PROVEN.json'],
             'unions3': ['continuation_b12/exact_union_v3.py'], 'findings': ['span_findings.py'],
             'translations': []}
    m.require(kind in extra, 'Importador no enumerado')
    paths.extend(extra[kind])
    registries = []
    if kind in ('variables2','variables3'):
        paths += ['continuation_b10/unsupported_audit/marker_mapping_v2.py',
                  'continuation_b10/unsupported_audit/VALIDATION_MARKER_MAPPING_V2.json']
        registries.append('continuation_b10/assembly/ROLE_REGISTRY_B10_v2.json')
    if kind == 'variables3': registries.append('continuation_b12/variables/ROLE_REGISTRY_B12_v3_APPROVED.json')
    paths.extend(registries)
    entries = [{'root':'memory','path':p} for p in paths]
    entries.extend({'root':'project','path':p} for p in (
        'desarrollo/pruebas_R38/vocab/package.py','desarrollo/pruebas_R39/vocab/build_r39.py',
        'desarrollo/pruebas_R37/save_supplement.py'))
    for relative in registries:
        registry = json.loads((MEM/relative).read_text())
        entries.extend({'root':'project','path':p['path']} for p in registry['second_read_evidence'])
    unique = {(entry['root'],entry['path']):entry for entry in entries}
    return [{**entry,'sha256':m.file_sha(finite.binding_path(entry))} for _,entry in sorted(unique.items())]


def schema_signature(c):
    return m.digest([tuple(r) for r in c.execute("SELECT type,name,tbl_name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name")])


def recovery_table_state(c, table, pid):
    h=hashlib.sha256(); count=0
    for row in finite.ordered_rows(c,table):
        values=list(row)
        if table=='koto_imports' and row['packet_id']==pid:
            result=json.loads(row['result_json']);result['backup']['path']='VERIFIED_PREDECESSOR_TRANSPORT'
            values[row.keys().index('result_json')]=m.canonical(result)
        data=finite.encoded_row(values);h.update(len(data).to_bytes(8,'little'));h.update(data);count+=1
    return {'rows':count,'sha256':h.hexdigest()}


def recover_committed_import(db, packet_path, kind, intent):
    """Una adopción huérfana se acredita por replay completo, no por su etiqueta."""
    m.require(source_bindings(kind)==intent['source_bindings'], 'El código cambió después del intento interrumpido')
    packet=json.loads(packet_path.read_text());ph=m.digest(packet)
    m.require(ph==intent['packet_sha256'] and m.file_sha(packet_path)==intent['packet_file_sha256'], 'Otro paquete que el intentado')
    backup=Path(intent['backup']['path']);record=json.loads(backup.with_suffix(backup.suffix+'.json').read_text())
    m.require(m.file_sha(backup)==record['compressed_sha256'] and
              record['source_sha256']==intent['database_sha256_before'], 'El respaldo del intento no coincide')
    with m.connect(db,True) as c:
        adopted=c.execute('SELECT * FROM koto_imports WHERE packet_sha256=?',(ph,)).fetchall()
        m.require(len(adopted)==1,'No hay una única adopción confirmada que recuperar')
        pid=adopted[0]['packet_id'];actual_result=json.loads(adopted[0]['result_json'])
        m.require(packet.get('packet_id',pid)==pid and actual_result['backup']['sha256']==record['source_sha256'],
                  'La adopción no parte del predecesor del intento')
    with tempfile.TemporaryDirectory(prefix='kotogba_B12_commit_recovery_') as td:
        replay=Path(td)/'replay.sqlite'
        with gzip.open(backup,'rb') as src,replay.open('wb') as out:shutil.copyfileobj(src,out,1024*1024)
        m.require(m.file_sha(replay)==intent['database_sha256_before'], 'Restauración de predecesor distinta')
        backups=replay.with_name(replay.name+'.backups');backups.mkdir()
        os.link(backup,backups/backup.name);shutil.copyfile(backup.with_suffix(backup.suffix+'.json'),backups/(backup.name+'.json'))
        replay_result=invoke(replay,packet_path,kind)
        m.require(replay_result['packet_id']==pid and not replay_result['already_applied'], 'Replay no produjo la adopción esperada')
        with m.connect(db,True) as actual,m.connect(replay,True) as expected:
            m.require(schema_signature(actual)==schema_signature(expected),'El esquema diverge del replay')
            m.require(finite.tables(actual)==finite.tables(expected),'Inventario de tablas distinto del replay')
            compared={}
            for table in finite.tables(actual):
                a=recovery_table_state(actual,table,pid);b=recovery_table_state(expected,table,pid)
                m.require(a==b,'La memoria contiene cambios ajenos al intento: '+table);compared[table]=a
    return actual_result, {'passed':True,'method':'restore_predecessor_replay_and_compare_all_tables',
        'only_normalized_field':'koto_imports[current_packet].result_json.backup.path',
        'tables':compared,'source_bindings_verified':True,'original_memory_not_rewritten':True}


def adopt(db, packet_path, kind, *, ledger_path=None, fault_hook=None):
    db,packet_path=Path(db).resolve(),Path(packet_path).resolve()
    m.require(packet_path.is_relative_to(MEM.resolve()), 'Paquete fuera del árbol de memoria')
    packet=json.loads(packet_path.read_text());ph=m.digest(packet)
    ledger_path=Path(ledger_path or HERE/'ADOPTION_LEDGER.json').resolve()
    pending=ledger_path.with_name(ledger_path.name+'.pending.json')
    if ledger_path.exists():ledger=json.loads(ledger_path.read_text())
    else:
        m.require(m.file_sha(db)==B11_SHA256,'La primera adopción debe partir de B11 exacta')
        ledger={'schema':'kotogba.B12_adoption_ledger/1','baseline_sha256':B11_SHA256,
                'baseline_state':state(db),'operations':[],'bound_files':[]}
        persist_json(ledger_path,ledger)
    last=ledger['operations'][-1] if ledger['operations'] else None
    expected_state=last['state_after'] if last else ledger['baseline_state']
    expected_sha=last['database_sha256_after'] if last else ledger['baseline_sha256']
    before,before_sha=state(db),m.file_sha(db)
    matches_endpoint=before==expected_state and before_sha==expected_sha
    prior=[x for x in ledger['operations'] if x['packet_sha256']==ph]
    if prior:
        m.require(matches_endpoint and len(prior)==1 and packet.get('packet_id',prior[0]['packet_id'])==prior[0]['packet_id'],
                  'La memoria cambió fuera del libro o la repetición corresponde a otra identidad')
        with m.connect(db,True) as c:already=m._existing_import(c,prior[0]['packet_id'],ph)
        m.require(already is not None,'El libro registra una adopción ausente')
        reports=ledger_path.parent/'adoption_reports';reports.mkdir(exist_ok=True)
        persist_json(reports/f"revision_{prior[0]['state_after']['revision']:03d}_{prior[0]['kind']}.json",prior[0])
        if pending.exists():
            p=json.loads(pending.read_text())
            m.require(p['packet_sha256']==ph,'Existe un intento pendiente de otro paquete')
            pending.unlink()
        return {'packet_id':prior[0]['packet_id'],'state':before,'result':already,'ledger_path':str(ledger_path)}
    intent=json.loads(pending.read_text()) if pending.exists() else None
    if intent:
        m.require(intent['schema']=='kotogba.B12_adoption_intent/1' and intent['packet_sha256']==ph and
                  intent['packet_file_sha256']==m.file_sha(packet_path) and intent['kind']==kind and
                  intent['state_before']==expected_state and intent['database_sha256_before']==expected_sha and
                  intent['predecessor_ledger_sha256']==m.file_sha(ledger_path), 'El intento pendiente no corresponde al libro y paquete actuales')
        m.require(intent['database_id']==packet.get('base',{}).get('database_id',packet.get('contract',{}).get('database_id',intent['database_id'])),
                  'Identidad de memoria distinta del intento')
        m.require(source_bindings(kind)==intent['source_bindings'],'Cambió el código o registro del intento pendiente')
    recovery=None
    if not matches_endpoint:
        m.require(intent is not None,'La memoria cambió fuera del libro y no hay intento recuperable')
        result,recovery=recover_committed_import(db,packet_path,kind,intent)
        before,before_sha=intent['state_before'],intent['database_sha256_before']
    else:
        if intent is None:
            seed_initial_backup(db,packet);backup,manifest=prepare_compressed_predecessor(db,packet)
            with m.connect(db,True) as c:database_id=m.meta(c,'database_id')
            intent={'schema':'kotogba.B12_adoption_intent/1','database_id':database_id,
                'game_id':before['game_id'],'base_sha256':before['base_sha256'],
                'kind':kind,'packet_file':str(packet_path.relative_to(MEM)),'packet_file_sha256':m.file_sha(packet_path),
                'packet_sha256':ph,'state_before':before,'database_sha256_before':before_sha,
                'predecessor_ledger_sha256':m.file_sha(ledger_path),'source_bindings':source_bindings(kind),
                'backup':{'path':str(backup),'manifest_sha256':m.file_sha(manifest)}}
            persist_json(pending,intent)
        if fault_hook:fault_hook('before_import')
        result=invoke(db,packet_path,kind)
        if fault_hook:fault_hook('after_import_before_ledger')
    with m.connect(db, True) as c:
        stored = c.execute('SELECT result_json FROM koto_imports WHERE packet_id=?', (result['packet_id'],)).fetchone()
        recorded = json.loads(stored[0])
    backup = Path(recorded['backup']['path'])
    archive = None
    if backup.exists() and backup.suffix == '.sqlite': archive = archive_backups.archive(backup)
    after = state(db); after_sha = m.file_sha(db)
    operation = {'packet_id': result['packet_id'], 'packet_sha256': result['packet_sha256'],
                 'packet_file': str(packet_path.relative_to(MEM)), 'packet_file_sha256': m.file_sha(packet_path),
                 'kind': kind, 'state_before': before, 'state_after': after,
                 'database_sha256_before': before_sha, 'database_sha256_after': after_sha,
                 'result_sha256': m.digest(recorded), 'result': recorded,
                 'compressed_backup_record': archive,'adoption_intent':intent,
                 'post_commit_recovery':recovery}
    existing = [x for x in ledger['operations'] if x['packet_id'] == result['packet_id']]
    if existing:
        m.require(existing[0]['packet_sha256'] == result['packet_sha256'] and result.get('already_applied'), 'Adopción duplicada distinta')
    else: ledger['operations'].append(operation)
    relative = str(packet_path.relative_to(MEM))
    if relative not in ledger['bound_files']: ledger['bound_files'].append(relative)
    for item in intent['source_bindings']:
        binding={key:item[key] for key in ('root','path')}
        if binding not in ledger['bound_files']:ledger['bound_files'].append(binding)
    persist_json(ledger_path, ledger)
    if fault_hook:fault_hook('after_ledger_before_report')
    reports = ledger_path.parent / 'adoption_reports'; reports.mkdir(exist_ok=True)
    persist_json(reports / f"revision_{after['revision']:03d}_{kind}.json", operation)
    pending.unlink()
    return {'packet_id': result['packet_id'], 'state': after, 'result': recorded,
            'ledger_path': str(ledger_path)}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--db', type=Path, required=True); p.add_argument('--packet', type=Path, required=True)
    p.add_argument('--kind', choices=['review','variables2','variables3','variable_spanish','raw_coordinates','unions3','findings','translations'], required=True)
    p.add_argument('--ledger',type=Path)
    a = p.parse_args(); print(json.dumps(adopt(a.db, a.packet, a.kind,ledger_path=a.ledger), ensure_ascii=False))
