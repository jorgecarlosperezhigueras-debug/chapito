#!/usr/bin/env python3
"""Publica una SQLite cerrada y comprimida, con recuperación byte a byte.

La copia transaccional puede estar fuera del árbol sincronizado. El transporte
canónico contiene todo el diccionario, corpus, catálogo e historial de revisión.
"""
from pathlib import Path
import sys
HERE=Path(__file__).resolve().parent;MEM=HERE.parent
if str(MEM) not in sys.path:sys.path.insert(0,str(MEM))
import argparse
import fcntl
import gzip
import hashlib
import json
import os
import re
import sqlite3
import tempfile
import memory as m
from continuation_b12 import export_runtime_v4 as projection
PROJECTION_EXPORTER_SHA256='bdacb08b8ce00dc5eeed8005309a504b5070782beda79c0734d5616f9218646b'
B11_PUBLISHER_DERIVATION_SHA256='a8fa6197d82cbc264096a9c4e4fd537ad95da10bbc9ee91a255960150c6f3671'


def validate_paths(db,output,base_kc15=None,report_path=None):
    db,output=Path(db),Path(output)
    sidecars=[output.with_suffix(output.suffix+'.json')]
    if report_path is not None:sidecars.append(Path(report_path))
    destinations=[output,output.with_name(output.name+'.writing')]+sidecars
    destinations.extend(path.with_name(path.name+'.writing') for path in sidecars)
    sources=[db]+([Path(base_kc15)] if base_kc15 is not None else [])
    all_paths=[path.resolve() for path in destinations+sources]
    m.require(len(all_paths)==len(set(all_paths)),'Snapshot, actas, temporales o entradas comparten una ruta')
    protected={Path(__file__).resolve(),Path(projection.__file__).resolve(),projection.CONTRACT_FILE.resolve(),
               db.with_name(db.name+'.lock').resolve()}
    if projection.CONTRACT_FILE.is_file():
        contract=json.loads(projection.CONTRACT_FILE.read_text())
        protected.update(projection.finite.binding_path(item) for item in contract['bound_files'])
    m.require(not({path.resolve() for path in destinations}&protected),'El snapshot o sus actas sobrescribirían una dependencia')


def publish(db,output,expected_runtime_sha256,base_kc15=None,expected_kc15_sha256=None,report_path=None):
    db,output=Path(db),Path(output)
    validate_paths(db,output,base_kc15,report_path)
    m.require(re.fullmatch('[0-9a-f]{64}',PROJECTION_EXPORTER_SHA256) is not None and
              m.file_sha(Path(projection.__file__))==PROJECTION_EXPORTER_SHA256,'Exportador B12 no sellado o alterado')
    m.require(m.file_sha(MEM/'continuation_b11/publish_snapshot_v3.py')==B11_PUBLISHER_DERIVATION_SHA256,
              'La derivación histórica del publicador ha cambiado')
    m.require((base_kc15 is None)==(expected_kc15_sha256 is None),
              'La recuperación pareada exige base KC15 y hash final KC15')
    if base_kc15 is not None:base_kc15=Path(base_kc15)
    m.require(not output.exists() and output.name.endswith('.sqlite.gz'),'Usar un destino nuevo terminado en .sqlite.gz')
    output.parent.mkdir(parents=True,exist_ok=True)
    with db.with_name(db.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        with tempfile.TemporaryDirectory(prefix='kotogba-final-snapshot-') as td:
            work=Path(td);snapshot=work/'memory.sqlite'
            with m.connect(db,True) as source,sqlite3.connect(snapshot) as destination:source.backup(destination)
            validation=m.validate(snapshot,deep=True)
            companion=work/'BPRJ_01.kc15' if base_kc15 else None
            runtime=projection.export_runtime(snapshot,work/'BPRJ_01.kl29',
                                     base_kc15=base_kc15,output_kc15=companion)
            m.require(runtime['output_sha256']==expected_runtime_sha256,'El snapshot no reproduce la candidata declarada')
            paired=None
            if companion is not None:
                m.require(runtime.get('binary_pair_validated') is True and runtime.get('installable') is False and
                          m.file_sha(companion)==expected_kc15_sha256,
                          'El snapshot no reproduce el compañero KC15 declarado')
                paired={'base_kc15':{'path':str(base_kc15),'bytes':base_kc15.stat().st_size,
                                    'sha256':m.file_sha(base_kc15)},
                        'kl29_sha256':runtime['output_sha256'],
                        'kc15_sha256':m.file_sha(companion),'kc15_bytes':companion.stat().st_size,
                        'binary_pair_validated':True,'installable':False,'status':'DEVELOPMENT_NOT_INSTALLABLE'}
            m.require(not runtime.get('companion_export_required') or paired is not None,
                      'No se publica una recuperación final con traducciones sin compañero KC15')
            with m.connect(snapshot,True) as c:
                database_id=m.meta(c,'database_id')
                states=[dict(r) for r in c.execute('SELECT game_id,base_sha256,revision,runtime_revision FROM koto_games')]
            stage=output.with_name(output.name+'.writing');m.require(not stage.exists(),'Existe un transporte interrumpido')
            h=hashlib.sha256();n=0
            with snapshot.open('rb') as src,stage.open('wb') as raw:
                with gzip.GzipFile(filename='',fileobj=raw,mode='wb',compresslevel=6,mtime=0) as dest:
                    while chunk:=src.read(1<<20):h.update(chunk);n+=len(chunk);dest.write(chunk)
                raw.flush();os.fsync(raw.fileno())
            verify=hashlib.sha256();restored_bytes=0
            with gzip.open(stage,'rb') as src:
                while chunk:=src.read(1<<20):verify.update(chunk);restored_bytes+=len(chunk)
            m.require(n==restored_bytes and verify.digest()==h.digest() and m.file_sha(snapshot)==h.hexdigest(),
                      'La descompresión no restaura la SQLite exacta')
            manifest={'schema':'kotogba.compressed_backup/1','role':'canonical_complete_memory_snapshot',
                'status':'DEVELOPMENT_NOT_INSTALLABLE','installable':False,
                'source_name':output.name[:-3],'source_sha256':h.hexdigest(),'source_bytes':n,
                'compressed_sha256':m.file_sha(stage),'compressed_bytes':stage.stat().st_size,
                'sqlite_integrity_check':'ok','decompression_sha256_verified':True,
                'memory_schema':m.SCHEMA,'database_id':database_id,'game_states':states,
                'runtime_sha256':runtime['output_sha256'],'runtime_bytes':runtime['output_bytes'],
                'restore_command':'python3 desarrollo/pruebas_HITO_B/memory/memory.py restore-copy --backup '+str(output)+' --output /ruta/elegida/kotogba.sqlite',
                'personal_collections_included':False,
                'projection_exporter':{'path':'desarrollo/pruebas_HITO_B/memory/continuation_b12/export_runtime_v4.py',
                    'sha256':m.file_sha(HERE/'export_runtime_v4.py'),'schema':'kotogba.memory_runtime_export/4',
                    'memory_revision':runtime['versioned_export_contract']['memory_revision'],'runtime_revision':runtime['versioned_export_contract']['runtime_revision'],'B11_exporter_sha256':projection.DERIVATION_B11_SHA256},
                'snapshot_publisher':{'path':'desarrollo/pruebas_HITO_B/memory/continuation_b12/publish_snapshot_v4.py',
                    'sha256':m.file_sha(Path(__file__)),'B11_derivation_source_sha256':B11_PUBLISHER_DERIVATION_SHA256}}
            if paired is not None:manifest['paired_runtime']=paired
            os.replace(stage,output);m.write_json(output.with_suffix(output.suffix+'.json'),manifest)
            return {'manifest':manifest,'validation':validation,'runtime_export':{
                k:v for k,v in runtime.items() if k!='changed_documents'},'output':str(output)}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--db',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--runtime-sha256',required=True)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--base-kc15',type=Path)
    p.add_argument('--kc15-sha256')
    a=p.parse_args()
    report=publish(a.db,a.output,a.runtime_sha256,a.base_kc15,a.kc15_sha256,a.report);m.write_json(a.report,report)
    print(json.dumps({'output':report['output'],'manifest':report['manifest']},ensure_ascii=False,indent=2))
