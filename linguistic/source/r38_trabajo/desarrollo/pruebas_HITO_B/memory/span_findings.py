#!/usr/bin/env python3
"""Disposiciones de revisión para candidatos que abarcan varias apariciones.

Sólo registra juicios y sus pruebas: una corrección declarada debe existir ya
en la memoria. No cambia texto, segmentación, fichas ni datos personales.
"""
from collections import Counter
from pathlib import Path
import argparse
import fcntl
import json
import memory as m

SCHEMA='kotogba.span_findings/1'
DDL='''CREATE TABLE IF NOT EXISTS koto_span_findings(
 finding_id TEXT PRIMARY KEY, game_id TEXT NOT NULL REFERENCES koto_games(game_id),
 document_id TEXT NOT NULL REFERENCES koto_documents(document_id), display_start INTEGER NOT NULL,
 display_end INTEGER NOT NULL, revision INTEGER NOT NULL, decision TEXT NOT NULL,
 reason TEXT NOT NULL, reviewer TEXT NOT NULL, evidence_json TEXT NOT NULL,
 packet_id TEXT NOT NULL REFERENCES koto_imports(packet_id))'''
ITEM_KEYS={'finding_id','source_offset','source_sha256','start','end','visible','before','expected_after',
           'expected_revision','decision','reason','reviewer','evidence'}


def checked_translation_context(conn, document, evidence):
    """El juicio conserva la versión castellana que realmente se revisó.

    Los paquetes anteriores al suplemento no tenían estos campos: sólo son
    compatibles con revisión0. Un contexto posterior necesita hash explícito.
    La comprobación no cambia ni migra los dictámenes históricos.
    """
    current=m.document_translations.state(conn,document)
    revision=evidence.get('translation_revision',0)
    expected_hash=evidence.get('translation_sha256')
    m.require(type(revision)is int and revision==current['revision'],
              'El dictamen se revisó con otra versión de la traducción')
    m.require((revision==0 and expected_hash is None) or expected_hash==current['sha256'],
              'La traducción del dictamen no coincide con su contexto revisado')
    if 'translation_packet_id' in evidence:
        m.require(evidence['translation_packet_id']==current['packet_id'],
                  'La procedencia de la traducción del dictamen es distinta')
    return {'translation_revision':current['revision'],'translation_sha256':current['sha256'],
            'translation_packet_id':current['packet_id']}


def ingest(db,path,fault_hook=None):
    p=json.loads(Path(path).read_text())
    m.require(set(p)=={'schema','game_id','base_sha256','inventory_code','complete','candidate_count','items'},
              'Paquete de dictámenes incompatible')
    m.require(p['schema']==SCHEMA and type(p['complete'])is bool,'Esquema de dictámenes incompatible')
    m.require(isinstance(p['inventory_code'],str) and p['inventory_code'].strip(),'Inventario sin identidad')
    m.require(isinstance(p['items'],list) and p['items'] and type(p['candidate_count'])is int,
              'Inventario de dictámenes vacío o inválido')
    if p['complete']:m.require(len(p['items'])==p['candidate_count'],'Dictámenes incompletos')
    ph=m.digest(p);pid='span_findings:'+ph;db=Path(db);game=p['game_id']
    with db.with_name(db.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);c=m.connect(db)
        try:
            m.check_schema(c);c.execute('BEGIN IMMEDIATE')
            prior=m._existing_import(c,pid,ph)
            if prior:c.rollback();return prior
            g=c.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
            m.require(g and g['base_sha256']==p['base_sha256'],'Dictámenes para otra base')
            c.execute(DDL);seen=set();prepared=[]
            for item in p['items']:
                m.require(set(item)==ITEM_KEYS,'Campos de dictamen incompatibles')
                fid=item['finding_id'];m.require(isinstance(fid,str) and fid and fid not in seen,'Identidad de dictamen duplicada');seen.add(fid)
                did=m.document_id(game,item['source_offset']);d=c.execute('SELECT * FROM koto_documents WHERE document_id=?',(did,)).fetchone()
                m.require(d and d['source_sha256']==item['source_sha256'],'Documento de otro inventario')
                lo,hi=item['start'],item['end']
                m.require(type(lo)is int and type(hi)is int and 0<=lo<hi<=len(d['japanese'].encode()),'Rango de dictamen inválido')
                m.require(d['japanese'].encode()[lo:hi].decode()==item['visible'],'Texto de dictamen divergente')
                m.require(item['decision'] in ('correct','corrected','unresolved'),'Dictamen pendiente de clasificar')
                for key in ('reason','reviewer'):
                    m.require(isinstance(item[key],str) and item[key].strip(),'Dictamen sin justificación o responsable')
                m.require(isinstance(item['evidence'],dict) and item['evidence'],'Dictamen sin evidencia')
                checked_translation_context(c,d,item['evidence'])
                old=c.execute('SELECT revision FROM koto_span_findings WHERE finding_id=?',(fid,)).fetchone()
                m.require(type(item['expected_revision'])is int and item['expected_revision']==(old[0] if old else 0),
                          'Versión de dictamen obsoleta')
                after=[s for s in m._active_spans(c,did) if s['display_byte_start']<hi and lo<s['display_byte_end']]
                m.require(after==item['expected_after'] and after,'El análisis activo cambió después de la revisión')
                m.require(isinstance(item['before'],list) and item['before'],'Falta preimagen del candidato')
                for previous in item['before']:
                    oid=m.occurrence_id(did,previous['display_byte_start'],previous['display_byte_end'])
                    row=m._occurrence(c,oid)
                    expected_hash=m.digest(previous['card'])
                    variant=c.execute('SELECT variant_id FROM koto_card_variants WHERE payload_sha256=?',(expected_hash,)).fetchone()
                    m.require(variant is not None,'La ficha de preimagen nunca existió')
                    history={row['variant_id'],row['baseline_variant_id']}
                    for h in c.execute('SELECT before_json,after_json FROM koto_changes WHERE occurrence_id=?',(oid,)):
                        for payload in h:
                            state=json.loads(payload)
                            if state:history.add(state['variant_id'])
                    m.require(variant[0] in history,'Preimagen ajena al historial de la aparición')
                if item['decision']=='correct':m.require(after==item['before'],'Se declara conservado un análisis que cambió')
                if item['decision']=='corrected':
                    m.require(after!=item['before'],'La corrección todavía no está aplicada')
                    ids=item['evidence'].get('correction_packet_ids',[])
                    m.require(ids and all(c.execute('SELECT 1 FROM koto_imports WHERE packet_id=?',(x,)).fetchone() for x in ids),
                              'La corrección no referencia una importación confirmada')
                prepared.append((did,item,after))
            backup=m._backup(c,db,ph);revision=g['revision']+1
            result={'schema':'kotogba.span_finding_import/1','packet_id':pid,'packet_sha256':ph,
                    'game_id':game,'inventory_code':p['inventory_code'],'candidates':len(prepared),
                    'counts':dict(Counter(x['decision'] for _,x,_ in prepared)),'backup':backup,
                    'runtime_content_changed':False,'already_applied':False}
            c.execute('INSERT INTO koto_imports VALUES(?,?,?,?)',(pid,ph,game,m.canonical(result)));m._store_packet(c,pid,p)
            updated=set()
            for n,(did,item,after) in enumerate(prepared,1):
                c.execute('INSERT INTO koto_span_findings VALUES(?,?,?,?,?,?,?,?,?,?,?) '
                    'ON CONFLICT(finding_id) DO UPDATE SET revision=excluded.revision,decision=excluded.decision,'
                    'reason=excluded.reason,reviewer=excluded.reviewer,evidence_json=excluded.evidence_json,packet_id=excluded.packet_id',
                    (item['finding_id'],game,did,item['start'],item['end'],item['expected_revision']+1,
                     item['decision'],item['reason'],item['reviewer'],m.canonical(item['evidence']),pid))
                if item['decision']!='unresolved':
                    for s in after:
                        oid=m.occurrence_id(did,s['display_byte_start'],s['display_byte_end'])
                        if oid in updated:continue
                        row=m._occurrence(c,oid);updated.add(oid)
                        c.execute('UPDATE koto_occurrences SET status="reviewed",reviewer=?,reason=?,revision=revision+1 WHERE occurrence_id=?',
                            (item['reviewer'],item['reason'],oid))
                        m._log_change(c,game,revision,pid,oid,m._state(row),m._state(m._occurrence(c,oid)))
                if fault_hook:fault_hook('after_item',n)
            c.execute('UPDATE koto_games SET revision=? WHERE game_id=?',(revision,game))
            c.commit();return result
        except BaseException:c.rollback();raise
        finally:c.close()


if __name__=='__main__':
    a=argparse.ArgumentParser(description=__doc__);a.add_argument('--db',type=Path,required=True)
    a.add_argument('--packet',type=Path,required=True);a.add_argument('--report',type=Path);p=a.parse_args()
    r=ingest(p.db,p.packet)
    if p.report:m.write_json(p.report,r)
    print(json.dumps(r,ensure_ascii=False,indent=2))
