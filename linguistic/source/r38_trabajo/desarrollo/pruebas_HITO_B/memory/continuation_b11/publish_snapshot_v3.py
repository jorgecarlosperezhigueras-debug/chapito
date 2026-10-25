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
import sqlite3
import tempfile
import memory as m
from continuation_b11 import export_runtime_v3 as projection
B10_PUBLISHER_DERIVATION_SHA256='dac45571f204b7a3bec1691aa9014fb61dfe5f1520e3a9fb6da7696e6abb57c6'


def publish(db,output,expected_runtime_sha256,base_kc15=None,expected_kc15_sha256=None):
    db,output=Path(db),Path(output)
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
                m.require(runtime.get('installable_pair_validated') is True and
                          m.file_sha(companion)==expected_kc15_sha256,
                          'El snapshot no reproduce el compañero KC15 declarado')
                paired={'base_kc15':{'path':str(base_kc15),'bytes':base_kc15.stat().st_size,
                                    'sha256':m.file_sha(base_kc15)},
                        'kl29_sha256':runtime['output_sha256'],
                        'kc15_sha256':m.file_sha(companion),'kc15_bytes':companion.stat().st_size,
                        'installable_pair_validated':True}
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
                'source_name':output.name[:-3],'source_sha256':h.hexdigest(),'source_bytes':n,
                'compressed_sha256':m.file_sha(stage),'compressed_bytes':stage.stat().st_size,
                'sqlite_integrity_check':'ok','decompression_sha256_verified':True,
                'memory_schema':m.SCHEMA,'database_id':database_id,'game_states':states,
                'runtime_sha256':runtime['output_sha256'],'runtime_bytes':runtime['output_bytes'],
                'restore_command':'python3 desarrollo/pruebas_HITO_B/memory/memory.py restore-copy --backup '+str(output)+' --output /ruta/elegida/kotogba.sqlite',
                'personal_collections_included':False,
                'projection_exporter':{'path':'desarrollo/pruebas_HITO_B/memory/continuation_b11/export_runtime_v3.py',
                    'sha256':m.file_sha(HERE/'export_runtime_v3.py'),'schema':'kotogba.memory_runtime_export/3',
                    'memory_revision':39,'runtime_revision':27,'B10_exporter_sha256':projection.B10_EXPORTER_SHA256},
                'snapshot_publisher':{'path':'desarrollo/pruebas_HITO_B/memory/continuation_b11/publish_snapshot_v3.py',
                    'sha256':m.file_sha(Path(__file__)),'B10_derivation_source_sha256':B10_PUBLISHER_DERIVATION_SHA256}}
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
    report=publish(a.db,a.output,a.runtime_sha256,a.base_kc15,a.kc15_sha256);m.write_json(a.report,report)
    print(json.dumps({'output':report['output'],'manifest':report['manifest']},ensure_ascii=False,indent=2))
