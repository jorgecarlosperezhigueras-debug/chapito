#!/usr/bin/env python3
"""Catálogo de fichas del runtime con ordinales que sólo pueden ampliarse.

C13 permanece como autoridad histórica. La transición explícita desde las
primeras herramientas B no declara adoptadas sus exportaciones intermedias.
A partir de este catálogo se retiene toda ficha emitible, aunque se corrija.
"""
from pathlib import Path
import argparse
import fcntl
import json
import struct

DDL=(
'''CREATE TABLE IF NOT EXISTS koto_runtime_cards(
 document_id TEXT NOT NULL REFERENCES koto_documents(document_id),
 ordinal INTEGER NOT NULL, variant_id TEXT NOT NULL REFERENCES koto_card_variants(variant_id),
 origin TEXT NOT NULL, PRIMARY KEY(document_id,ordinal))''',
'''CREATE TRIGGER IF NOT EXISTS koto_runtime_cards_no_update BEFORE UPDATE ON koto_runtime_cards
 BEGIN SELECT RAISE(ABORT,'No se reasignan fichas ni ordinales publicados'); END''',
'''CREATE TRIGGER IF NOT EXISTS koto_runtime_cards_no_delete BEFORE DELETE ON koto_runtime_cards
 BEGIN SELECT RAISE(ABORT,'No se retiran fichas ni ordinales publicados'); END''')


def present(c):
    return c.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='koto_runtime_cards'").fetchone() is not None


def create(c):
    for sql in DDL:c.execute(sql)


def seed_baseline(c):
    c.execute('INSERT INTO koto_runtime_cards SELECT document_id,ordinal,variant_id,? FROM koto_document_cards',
              ('adopted_base',))


def sync_document(c,did,origin='validated_content_import'):
    import memory as m
    m.require(present(c),'La memoria necesita inicializar explícitamente el catálogo de fichas del runtime')
    rows=c.execute('SELECT ordinal,variant_id FROM koto_runtime_cards WHERE document_id=? ORDER BY ordinal',(did,)).fetchall()
    seen={r['variant_id'] for r in rows};ordinal=len(rows)
    m.require([r['ordinal'] for r in rows]==list(range(ordinal)),'Hay un hueco en los ordinales publicados')
    for row in c.execute('SELECT variant_id FROM koto_occurrences WHERE document_id=? AND active=1 ORDER BY display_start,display_end',(did,)):
        if row[0] in seen:continue
        m.require(ordinal<256,'Las fichas actuales e históricas exceden la capacidad de 256 fichas del formato DS')
        c.execute('INSERT INTO koto_runtime_cards VALUES(?,?,?,?)',(did,ordinal,row[0],origin))
        seen.add(row[0]);ordinal+=1


def cards(c,did):
    import memory as m
    m.require(present(c),'Falta el catálogo estable del runtime; exportación bloqueada para no perder identidades')
    return [json.loads(r[0]) for r in c.execute('SELECT v.card_json FROM koto_runtime_cards r '
        'JOIN koto_card_variants v USING(variant_id) WHERE document_id=? ORDER BY ordinal',(did,))]


def replace_inline(old,spans,retained):
    """Códec KL29 heredado, con el inventario explícito de fichas conservadas.

    El diseño binario, desplazamiento de tablas y deduplicación de cadenas son
    los de pruebas_R38/vocab/package.py. Sólo difiere la fuente de la lista de
    fichas: el catálogo persistente sustituye a 'C13 + enlaces activos'.
    """
    import memory as m
    kl=m.kl;parsed=kl.document(old);h=list(parsed['header']);ia=parsed['inline_at']
    m.require(retained[:len(parsed['cards'])]==parsed['cards'],'El catálogo perdió ordinales de la base adoptada')
    m.require(len(retained)<=256 and len(spans)<=512,'Capacidad de tablas del runtime excedida')
    index={};encoded=[]
    for i,card in enumerate(retained):index.setdefault(m.digest(card),i)
    for s in spans:
        key=m.digest(s['card']);m.require(key in index,'Ficha activa ausente del catálogo persistente')
        encoded.append((s['raw_start'],s['raw_end'],s['display_byte_start'],s['display_byte_end'],index[key],s['flags']))
    strings_at=ia+16+12*len(spans)+24*len(retained);delta=strings_at-h[8]
    doc=bytearray(old[:ia]+bytes(strings_at-ia)+old[h[8]:])
    def shift(at,wide=False):
        fmt='<I' if wide else '<H';v=struct.unpack_from(fmt,doc,at)[0]
        if v:
            m.require(h[8]<=v<len(old),'Puntero heredado fuera de la tabla de cadenas')
            struct.pack_into(fmt,doc,at,v+delta)
    for i in range(h[5]):shift(h[4]+i*12+4);shift(h[4]+i*12+8)
    for i in range(h[7]):shift(h[6]+i*32+12);shift(h[6]+i*32+16)
    for i in range(h[11]):shift(h[10]+i*16+8,True)
    strings={};at=strings_at
    while at<len(doc):
        end=doc.index(0,at);s=bytes(doc[at:end]).decode();strings.setdefault(s,(at,end-at));at=end+1
    def add(s):
        if not s:return 0,0
        if s in strings:return strings[s]
        b=s.encode()
        for previous,(off,n) in list(strings.items()):
            if previous.endswith(s):
                result=(off+n-len(b),len(b));strings[s]=result;return result
        result=(len(doc),len(b));doc.extend(b+b'\0');strings[s]=result;return result
    struct.pack_into('<8s4H',doc,ia,b'KOTOV032',len(spans),len(retained),0,0)
    for i,s in enumerate(encoded):kl.S.pack_into(doc,ia+16+12*i,*s)
    for i,card in enumerate(retained):
        fields=[]
        for key in ('surface','reading','meaning_es','explanation_es'):fields.extend(add(card[key]))
        kl.C.pack_into(doc,ia+16+12*len(spans)+24*i,card['identity'],*fields,card['type'],0)
    h[2]=len(doc);h[8]=strings_at;h[12]=h[12]+delta if h[12] else 0;h[3]=kl.fnv(doc[64:]);kl.H.pack_into(doc,0,*h)
    result=bytes(doc);current=kl.document(result)
    for key in ('raw','raw_hash','units','notes','fragments','words','spanish','japanese'):
        m.require(parsed[key]==current[key],f'El catálogo alteró {key}')
    m.require(current['cards']==retained and current['spans']==spans,'Relectura de fichas o enlaces divergente')
    return result,delta


def validate_document(c,d,parsed):
    import memory as m
    rs=c.execute('SELECT ordinal,variant_id FROM koto_runtime_cards WHERE document_id=? ORDER BY ordinal',
                 (d['document_id'],)).fetchall()
    m.require([r['ordinal'] for r in rs]==list(range(len(rs))) and len(rs)<=256,'Catálogo con ordinales incompatibles')
    retained=cards(c,d['document_id'])
    m.require(retained[:len(parsed['cards'])]==parsed['cards'],'El catálogo perdió la base adoptada')
    ids={r['variant_id'] for r in rs}
    m.require(all(r[0] in ids for r in c.execute('SELECT variant_id FROM koto_occurrences WHERE document_id=? AND active=1',
              (d['document_id'],))),'Ficha activa no conservada')


def initialize(db,reason,fault_hook=None):
    import memory as m
    db=Path(db)
    m.require(reason.strip(),'La migración necesita justificar cuáles exportaciones fueron adoptadas')
    with db.with_name(db.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);c=m.connect(db)
        try:
            m.check_schema(c);c.execute('BEGIN IMMEDIATE')
            key='runtime_card_catalog_transition'
            old=c.execute('SELECT value FROM koto_meta WHERE key=?',(key,)).fetchone()
            if old:
                result=json.loads(old[0]);result['already_applied']=True;c.rollback();return result
            m.require(not present(c),'El catálogo ya está inicializado; no se pueden reasignar ordinales')
            games=[dict(r) for r in c.execute('SELECT game_id,base_sha256,revision,runtime_revision FROM koto_games')]
            packet={'schema':'kotogba.runtime_card_catalog_transition/1','basis':'adopted_base_and_current',
                'games':games,'reason':reason,'prior_intermediate_retired_variants':'Preservadas en la memoria e historial; no publicadas ni adoptadas.'}
            ph=m.digest(packet);pid='runtime_card_catalog:'+ph;backup=m._backup(c,db,ph)
            create(c);seed_baseline(c)
            docs=c.execute('SELECT * FROM koto_documents ORDER BY game_id,source_offset').fetchall()
            for n,d in enumerate(docs,1):
                sync_document(c,d['document_id'],pid)
                validate_document(c,d,m.kl.document(d['original_bytes']))
                if fault_hook:fault_hook('after_document',n)
            result={'schema':'kotogba.runtime_card_catalog_transition_result/1','packet_id':pid,'backup':backup,
                'documents':len(docs),'cards':c.execute('SELECT COUNT(*) FROM koto_runtime_cards').fetchone()[0],
                'basis':'adopted_base_and_current','already_applied':False,'personal_collections_opened':False}
            m.require(len(games)==1,'La transición de este checkpoint espera un solo juego')
            c.execute('INSERT INTO koto_imports VALUES(?,?,?,?)',(pid,ph,games[0]['game_id'],m.canonical(result)))
            m._store_packet(c,pid,packet)
            c.execute('INSERT INTO koto_meta VALUES(?,?)',(key,m.canonical(result)))
            c.execute('UPDATE koto_games SET revision=revision+1')
            if fault_hook:fault_hook('before_commit',len(docs))
            c.commit();return result
        except BaseException:c.rollback();raise
        finally:c.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--db',type=Path,required=True)
    p.add_argument('--initialize-adopted-base-and-current',action='store_true',required=True)
    p.add_argument('--reason',required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
    import memory as m
    r=initialize(a.db,a.reason);m.write_json(a.report,r);print(json.dumps(r,ensure_ascii=False,indent=2))
