#!/usr/bin/env python3
"""Registra el juicio individual sobre candidatos sin confundir flags y errores."""
from pathlib import Path
from collections import Counter
import argparse
import fcntl
import json
import memory as m

DDL='CREATE TABLE IF NOT EXISTS koto_review_resolutions(reason_id TEXT PRIMARY KEY REFERENCES koto_review_reasons(reason_id), revision INTEGER NOT NULL, decision TEXT NOT NULL, reason TEXT NOT NULL, reviewer TEXT NOT NULL, evidence_json TEXT NOT NULL, packet_id TEXT NOT NULL REFERENCES koto_imports(packet_id))'


def template(db,output,code='grammar_694',game='bprj1'):
    c=m.connect(db,True)
    try:
        g=c.execute('SELECT base_sha256 FROM koto_games WHERE game_id=?',(game,)).fetchone()
        rows=[]
        for r in c.execute('SELECT * FROM koto_review_reasons WHERE code=? ORDER BY document_id,occurrence_id',(code,)):
            details=json.loads(r['details_json'])['flag']
            m.require(r['occurrence_id'] and 'card' in details,'Este inventario no contiene candidatos de aparición')
            rows.append({'reason_id':r['reason_id'],'occurrence_id':r['occurrence_id'],
                'expected_identity':details['card']['identity'],'expected_revision':0,
                'decision':None,'reason':'','reviewer':'','evidence':{}})
        packet={'schema':'kotogba.review_dispositions/1','game_id':game,'base_sha256':g[0],
                'candidate_code':code,'complete':True,'items':rows}
        m.write_json(output,packet);return {'candidates':len(rows),'path':str(output)}
    finally:c.close()


def ingest(db,packet_path):
    packet=json.loads(Path(packet_path).read_text(encoding='utf-8'))
    m.require(set(packet)=={'schema','game_id','base_sha256','candidate_code','complete','items'},'Campos de disposiciones incompatibles')
    m.require(packet['schema']=='kotogba.review_dispositions/1','Esquema de disposiciones incompatible')
    m.require(type(packet['complete'])is bool and isinstance(packet['items'],list),'Cobertura de disposiciones inválida')
    ph=m.digest(packet);pid='dispositions:'+ph;game=packet['game_id'];db=Path(db)
    with db.with_name(db.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);c=m.connect(db)
        try:
            m.check_schema(c);c.execute('BEGIN IMMEDIATE')
            prior=m._existing_import(c,pid,ph)
            if prior:c.rollback();return prior
            g=c.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
            m.require(g and g['base_sha256']==packet['base_sha256'],'Las disposiciones son de otra base')
            c.execute(DDL)
            expected={r['reason_id']:r for r in c.execute('SELECT r.* FROM koto_review_reasons r '
                'JOIN koto_documents d USING(document_id) WHERE code=? AND d.game_id=?',(packet['candidate_code'],game))}
            ids=[i['reason_id'] for i in packet['items']]
            m.require(len(ids)==len(set(ids)),'Candidatos duplicados')
            m.require(set(ids)<=set(expected),'Candidato ajeno al inventario declarado')
            if packet['complete']:m.require(set(ids)==set(expected),'La revisión completa omite candidatos')
            prepared=[]
            for item in packet['items']:
                m.require(set(item)=={'reason_id','occurrence_id','expected_identity','expected_revision','decision','reason','reviewer','evidence'},'Campos de decisión inválidos')
                r=expected[item['reason_id']];flag=json.loads(r['details_json'])['flag']
                m.require(item['occurrence_id']==r['occurrence_id'] and item['expected_identity']==flag['card']['identity'],
                          'Identidad del candidato diferente')
                m.require(item['decision'] in ('correct','corrected','unresolved'),'Juicio individual pendiente')
                m.require(isinstance(item['reason'],str) and item['reason'].strip() and
                          isinstance(item['reviewer'],str) and item['reviewer'].strip() and isinstance(item['evidence'],dict),
                          'Falta justificación, responsable o evidencia')
                old=c.execute('SELECT * FROM koto_review_resolutions WHERE reason_id=?',(item['reason_id'],)).fetchone()
                m.require(type(item['expected_revision'])is int and item['expected_revision']==(old['revision'] if old else 0),
                          'Conflicto de versión de la disposición')
                occ=m._occurrence(c,item['occurrence_id'])
                original=json.loads(c.execute('SELECT card_json FROM koto_card_variants WHERE variant_id=?',(occ['baseline_variant_id'],)).fetchone()[0])
                if item['decision']=='correct':
                    m.require(occ['active'] and json.loads(occ['card_json'])==flag['card'],
                              'Se marca correcta una ficha que ya se sustituyó; documentar corrected')
                elif item['decision']=='corrected':
                    active=m._active_spans(c,r['document_id'])
                    current=[s for s in active if s['display_byte_start']<flag['hi'] and flag['lo']<s['display_byte_end']]
                    m.require(current and (not occ['active'] or json.loads(occ['card_json'])!=flag['card']),
                              'Se declara corregido un candidato sin cambio integrado')
                    m.require(item['evidence'],'Una corrección necesita referencia a su decisión o parche')
                prepared.append((item,occ))
            backup=m._backup(c,db,ph);revision=g['revision']+1
            result={'schema':'kotogba.dispositions_import/1','packet_id':pid,'packet_sha256':ph,
                    'game_id':game,'counts':dict(Counter(i['decision'] for i,_ in prepared)),
                    'candidate_code':packet['candidate_code'],'complete':packet['complete'],
                    'candidates':len(prepared),'backup':backup,'already_applied':False}
            c.execute('INSERT INTO koto_imports VALUES(?,?,?,?)',(pid,ph,game,m.canonical(result)));m._store_packet(c,pid,packet)
            for item,occ in prepared:
                c.execute('INSERT INTO koto_review_resolutions VALUES(?,?,?,?,?,?,?) '
                    'ON CONFLICT(reason_id) DO UPDATE SET revision=excluded.revision,decision=excluded.decision,'
                    'reason=excluded.reason,reviewer=excluded.reviewer,evidence_json=excluded.evidence_json,packet_id=excluded.packet_id',
                    (item['reason_id'],item['expected_revision']+1,item['decision'],item['reason'],item['reviewer'],m.canonical(item['evidence']),pid))
                c.execute('UPDATE koto_occurrences SET status=?,reviewer=?,reason=?,revision=revision+1 WHERE occurrence_id=?',
                    ('pending' if item['decision']=='unresolved' else 'reviewed',item['reviewer'],item['reason'],item['occurrence_id']))
                m._log_change(c,game,revision,pid,item['occurrence_id'],m._state(occ),m._state(m._occurrence(c,item['occurrence_id'])))
            c.execute('UPDATE koto_games SET revision=? WHERE game_id=?',(revision,game));c.commit();return result
        except BaseException:c.rollback();raise
        finally:c.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='command',required=True)
    t=sub.add_parser('template');t.add_argument('--db',type=Path,required=True);t.add_argument('--output',type=Path,required=True)
    t.add_argument('--code',default='grammar_694');t.add_argument('--game',default='bprj1')
    t=sub.add_parser('import');t.add_argument('--db',type=Path,required=True);t.add_argument('--packet',type=Path,required=True)
    a=p.parse_args();r=template(a.db,a.output,a.code,a.game) if a.command=='template' else ingest(a.db,a.packet)
    print(json.dumps(r,ensure_ascii=False,indent=2))
