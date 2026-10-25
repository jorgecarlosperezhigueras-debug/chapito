#!/usr/bin/env python3
"""Importa enlaces revisados a marcadores FD/F7 sin escribir sus valores.

Este canal separado sólo añade destinos que faltan. No permite convertir una
palabra en una variable, sobrescribir nombres o alterar enlaces anteriores.
"""
from collections import Counter, defaultdict
from pathlib import Path
import argparse
import fcntl
import json
import re

import memory as m

SCHEMA='kotogba.variable_links/1'
ROLES={'species','location','item','box_name','player_name','move','linked_participant_name',
       'place_name','battle_manner','battle_result','linked_trainer_name','defeated_count',
       'caught_count','trainer','rival_name','money_amount','item_quantity','item_count'}
ITEM_KEYS={'source_offset','raw_start','raw_end','display_byte_start','display_byte_end','visible',
           'before','raw_source_sha256','replacement','role','reason','provenance'}
PACKET_KEYS={'schema','game_id','base_sha256','reviewer','reason','provenance','items'}


def marker_mapping(document):
    """Assert the full ordered raw/display correspondence before linking one token."""
    raw=[(x.start(),x.group()[0],x.group()[1])
         for x in re.finditer(b'[\xfd\xf7][\x00-\x1f]',document['raw_source'])]
    display=list(re.finditer(r'\{(FD|F7):([0-9A-F]{2})\}',document['japanese']))
    m.require(len(raw)==len(display),'Marcadores binarios y de pantalla ambiguos')
    out={}
    for (at,code,token),match in zip(raw,display):
        m.require(code=={'FD':253,'F7':247}[match[1]] and token==int(match[2],16),
                  'Orden o identidad de marcadores divergente')
        lo=len(document['japanese'][:match.start()].encode());hi=lo+len(match[0].encode())
        out[(lo,hi)]=(at,at+2,match[0])
    return out


def ingest(db,packet_path,fault_hook=None):
    packet=json.loads(Path(packet_path).read_text(encoding='utf-8'))
    m.require(isinstance(packet,dict) and set(packet) in (PACKET_KEYS,PACKET_KEYS|{'packet_id'}),
              'Campos de paquete de variables incompatibles')
    m.require(packet['schema']==SCHEMA and isinstance(packet['game_id'],str),'Esquema de variables incompatible')
    m.require(isinstance(packet['base_sha256'],str) and re.fullmatch('[0-9a-f]{64}',packet['base_sha256']),
              'Identidad base inválida')
    for key in ('reviewer','reason'):
        m.require(isinstance(packet[key],str) and packet[key].strip(),'Falta revisión contextual explícita')
    m.require(isinstance(packet['provenance'],dict) and packet['provenance'],'Falta procedencia del paquete')
    m.require(isinstance(packet['items'],list) and packet['items'],'No hay variables para importar')
    game=packet['game_id'];ph=m.digest(packet);pid=packet.get('packet_id','variable_links:'+ph)
    m.require(isinstance(pid,str) and pid.strip(),'Identidad de paquete inválida')
    db=Path(db)
    with db.with_name(db.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);c=m.connect(db)
        try:
            m.check_schema(c);c.execute('BEGIN IMMEDIATE')
            prior=m._existing_import(c,pid,ph)
            if prior:c.rollback();return prior
            g=c.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
            m.require(g and g['base_sha256']==packet['base_sha256'],'Paquete para otra base o juego')
            prepared=[];seen=set();docs={};maps={};roles=Counter()
            for item in packet['items']:
                m.require(isinstance(item,dict) and set(item)==ITEM_KEYS,'Campos de variable incompatibles')
                m.require(type(item['source_offset']) is int and item['source_offset']>=0,'Fuente inválida')
                did=m.document_id(game,item['source_offset'])
                if did not in docs:
                    docs[did]=c.execute('SELECT * FROM koto_documents WHERE document_id=?',(did,)).fetchone()
                    m.require(docs[did] is not None,'Documento desconocido')
                    maps[did]=marker_mapping(docs[did])
                d=docs[did];lo,hi=item['display_byte_start'],item['display_byte_end']
                m.require(type(lo)is int and type(hi)is int and 0<=lo<hi<=len(d['japanese'].encode()),
                          'Intervalo UTF-8 inválido')
                m.require(type(item['raw_start']) is int and type(item['raw_end']) is int,'Intervalo fuente inválido')
                m.require(m.digest(d['raw_source'])==item['raw_source_sha256'],'Fuente binaria distinta')
                m.require(maps[did].get((lo,hi))==(item['raw_start'],item['raw_end'],item['visible']),
                          'El destino no corresponde al marcador binario exacto')
                m.require(d['japanese'].encode()[lo:hi].decode()==item['visible'],'Superficie distinta')
                m.require(item['role'] in ROLES,'Rol de variable no revisado en este esquema')
                m.require(isinstance(item['reason'],str) and item['reason'].strip() and
                          isinstance(item['provenance'],dict) and item['provenance'],'Falta revisión de la variable')
                replacement=item['replacement']
                m.require(isinstance(replacement,dict) and set(replacement)==m.REPLACEMENT_KEYS,
                          'Ficha de variable incompatible o identidad impuesta')
                m.require(replacement['type']==4 and type(replacement['type'])is int and
                          replacement['surface']==item['visible'] and replacement['reading']=='',
                          'No se permite escribir un valor o lectura fija para la variable')
                for key in ('meaning_es','explanation_es'):
                    m.require(isinstance(replacement[key],str) and replacement[key].strip() and
                              '\0' not in replacement[key], 'Ficha contextual vacía o inválida')
                before=[s for s in m._active_spans(c,did) if s['display_byte_start']<hi and lo<s['display_byte_end']]
                m.require(item['before']==before==[],'Ya existe un enlace en esta posición; no se sobrescribe')
                oid=m.occurrence_id(did,lo,hi)
                m.require(oid not in seen,'Variable repetida en el paquete');seen.add(oid)
                m.require(c.execute('SELECT 1 FROM koto_occurrences WHERE occurrence_id=?',(oid,)).fetchone() is None,
                          'Existe una identidad histórica en esa posición')
                u=c.execute('SELECT * FROM koto_units WHERE document_id=? AND display_start<=? '
                            'AND display_end>=? AND raw_start<=? AND raw_end>=?',
                            (did,lo,hi,item['raw_start'],item['raw_end'])).fetchall()
                m.require(u,'Variable sin correspondencia con las unidades originales')
                prepared.append((did,oid,item));roles[item['role']]+=1
            backup=m._backup(c,db,ph);revision=g['revision']+1
            report={'schema':'kotogba.variable_link_import/1','packet_id':pid,'packet_sha256':ph,
                    'game_id':game,'revision':revision,'added_links':len(prepared),'changed_documents':len(docs),
                    'roles':dict(roles),'backup':backup,'already_applied':False,
                    'existing_links_modified':0,'stored_personal_values':False,'source_units_modified':False}
            c.execute('INSERT INTO koto_imports VALUES(?,?,?,?)',(pid,ph,game,m.canonical(report)))
            m._store_packet(c,pid,packet)
            for n,(did,oid,item) in enumerate(prepared,1):
                card=m._new_card(c,item['replacement'])
                provenance={'packet_id':pid,'reviewer':packet['reviewer'],'role':item['role'],
                            'reason':item['reason'],'source_offset':item['source_offset'],
                            'evidence':item['provenance'],'value_policy':'runtime_frozen_visible_value_only'}
                variant=m.register_card(c,card,provenance)
                ordinal=c.execute('SELECT COALESCE(MAX(ordinal),-1)+1 FROM koto_occurrences WHERE document_id=?',(did,)).fetchone()[0]
                c.execute('INSERT INTO koto_occurrences VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                    (oid,did,ordinal,item['raw_start'],item['raw_end'],item['display_byte_start'],
                     item['display_byte_end'],1,variant,variant,1,'reviewed',packet['reviewer'],
                     item['reason'],m.canonical(provenance),1))
                m._log_change(c,game,revision,pid,oid,None,m._state(m._occurrence(c,oid)))
                if fault_hook:fault_hook('after_item',n)
            c.execute('UPDATE koto_games SET revision=?,runtime_revision=runtime_revision+1 WHERE game_id=?',(revision,game))
            m._validate_changed_documents(c,game,docs)
            if fault_hook:fault_hook('before_commit',len(prepared))
            c.commit();return report
        except BaseException:c.rollback();raise
        finally:c.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--db',type=Path,required=True)
    p.add_argument('--packet',type=Path,required=True);p.add_argument('--report',type=Path);a=p.parse_args()
    result=ingest(a.db,a.packet)
    if a.report:m.write_json(a.report,result)
    print(json.dumps(result,ensure_ascii=False,indent=2))
