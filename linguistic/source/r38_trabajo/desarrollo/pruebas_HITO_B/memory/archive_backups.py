#!/usr/bin/env python3
"""Comprime backups cerrados sólo después de verificar la descompresión exacta."""
from pathlib import Path
import argparse
import gzip
import hashlib
import json
import os
import sqlite3

import memory as m


def archive(path):
    path=Path(path);target=path.with_suffix(path.suffix+'.gz')
    m.require(path.suffix=='.sqlite' and path.name.startswith('before_'),'Sólo se archivan backups históricos')
    m.require(not target.exists(),'Ya existe un archivo comprimido con ese nombre')
    m.require(not path.with_name(path.name+'-journal').exists() and not path.with_name(path.name+'-wal').exists(),
              'No se archiva una base con transacción pendiente')
    with sqlite3.connect(f'file:{path.resolve()}?mode=ro',uri=True) as c:
        m.require(c.execute('PRAGMA quick_check').fetchone()[0]=='ok','Backup SQLite inválido')
    staging=target.with_name(target.name+'.writing');sha=hashlib.sha256();size=0
    try:
        with path.open('rb') as src,staging.open('wb') as raw:
            with gzip.GzipFile(filename='',fileobj=raw,mode='wb',compresslevel=6,mtime=0) as dst:
                while block:=src.read(1024*1024):
                    sha.update(block);size+=len(block);dst.write(block)
        check=hashlib.sha256();n=0
        with gzip.open(staging,'rb') as f:
            while block:=f.read(1024*1024):check.update(block);n+=len(block)
        m.require(size==n and sha.digest()==check.digest(),'La descompresión no restaura los bytes originales')
        m.require(m.file_sha(path)==sha.hexdigest(),'El backup cambió durante la compresión')
        record={'schema':'kotogba.compressed_backup/1','source_name':path.name,
                'source_sha256':sha.hexdigest(),'source_bytes':size,
                'compressed_sha256':m.file_sha(staging),'compressed_bytes':staging.stat().st_size,
                'sqlite_quick_check':'ok','decompression_sha256_verified':True}
        os.replace(staging,target);m.write_json(target.with_suffix(target.suffix+'.json'),record)
        # Only now is the reproducible uncompressed transport removed.
        path.unlink();return record
    except BaseException:
        if staging.exists():staging.unlink()
        raise


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);a=p.parse_args()
    records=[]
    for path in sorted(a.directory.glob('before_*.sqlite')):
        record=archive(path);records.append(record)
        print(json.dumps(record,ensure_ascii=False),flush=True)
    m.write_json(a.directory/'archive_index.json',{
        'schema':'kotogba.backup_archive_index/1','records':[json.loads(x.read_text()) for x in sorted(a.directory.glob('*.gz.json'))],
        'restore_command':'memory.py restore-copy --backup RUTA_SQLITE_O_GZ --output COPIA_NUEVA.sqlite'})
