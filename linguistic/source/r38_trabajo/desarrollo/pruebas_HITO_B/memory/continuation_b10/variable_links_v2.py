#!/usr/bin/env python3
"""Importa enlaces revisados a marcadores FD/F7 sin escribir sus valores.

Este canal separado sólo añade destinos que faltan. No permite convertir una
palabra en una variable, sobrescribir nombres o alterar enlaces anteriores.
"""
from collections import Counter, defaultdict
from pathlib import Path
import hashlib
import importlib.util
import sys
import argparse
import fcntl
import json
import re

MEMORY_DIR=Path(__file__).resolve().parents[1]
if str(MEMORY_DIR)not in sys.path:sys.path.insert(0,str(MEMORY_DIR))
import memory as m

SCHEMA='kotogba.variable_links/2'
BASE_INGESTER_SHA256='0cef79b9b0b7094e266ef6173b8a7053ca72276fb1156aadd757e1cb2caab7f7'
APPROVED_ROLE_REGISTRY_SHA256='ec23ce8c8819b9ce9f78680def6d065a85959dfe442f9824c8107345979b0aa9'  # Approved 388-item registry after final second read.
APPROVED_ADDITIONAL_ROLES=frozenset(('ability_name', 'badge_name', 'battle_effect', 'caught_species_threshold', 'coin_count', 'elapsed_minutes', 'elapsed_seconds', 'encounter_level', 'experience_points', 'floor_number', 'hall_of_fame_count', 'health_points_amount', 'hit_count', 'level_gain', 'level_value', 'magnitude_power', 'nature_name', 'participant_count', 'party_position', 'pc_owner_name', 'percentage', 'perish_count', 'pokedex_scope', 'pokemon_size_cm', 'pokemon_type', 'powder_quantity', 'pp_amount', 'prize_name', 'seen_count', 'ship_identifier', 'stat_name', 'status_name', 'stockpile_count', 'storage_container', 'trainer_class'))
REGISTRY_SCHEMA='kotogba.variable_link_roles/2'
CONTRACT_KEYS={'database_id','expected_memory_revision','expected_runtime_revision','role_registry_sha256'}
MARKER_LEXER_RELATIVE_PATH='continuation_b10/unsupported_audit/marker_mapping_v2.py'
APPROVED_MARKER_LEXER_SHA256='9ce191739bf8049e046991c6ecdeddd236851bc63c3664ab2d84093cf0112080'
MARKER_LEXER_PROOF_RELATIVE_PATH='continuation_b10/unsupported_audit/VALIDATION_MARKER_MAPPING_V2.json'
APPROVED_MARKER_LEXER_PROOF_SHA256='197f9a0de36f60b64fbb4bf3e9872398eecdd45b5187ea1a86d30b84f33b1692'
MARKER_LEXER_EXPORT='marker_mapping'
BASE_ROLES={'species','location','item','box_name','player_name','move','linked_participant_name',
       'place_name','battle_manner','battle_result','linked_trainer_name','defeated_count',
       'caught_count','trainer','rival_name','money_amount','item_quantity','item_count'}
ROLES=frozenset(BASE_ROLES)|APPROVED_ADDITIONAL_ROLES
ITEM_KEYS={'source_offset','raw_start','raw_end','display_byte_start','display_byte_end','visible',
           'before','raw_source_sha256','replacement','role','reason','provenance'}
PACKET_KEYS={'schema','game_id','base_sha256','reviewer','reason','provenance','items','contract'}


def _marker_mapping_v1(document):
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


def _file_sha(path):
    with Path(path).open('rb')as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def _approved_registry(path):
    m.require(Path(m.__file__).resolve()==(MEMORY_DIR/'memory.py').resolve(),'Módulo de memoria ajeno al proyecto')
    m.require(_file_sha(MEMORY_DIR/'variable_links.py')==BASE_INGESTER_SHA256,'El ingester v1 histórico ha cambiado')
    m.require(isinstance(APPROVED_ROLE_REGISTRY_SHA256,str) and
              re.fullmatch('[0-9a-f]{64}',APPROVED_ROLE_REGISTRY_SHA256),
              'Registro v2 todavía no aprobado ni fijado; no se importa')
    data=Path(path).read_bytes()
    m.require(hashlib.sha256(data).hexdigest()==APPROVED_ROLE_REGISTRY_SHA256,'Registro de roles distinto del aprobado')
    registry=json.loads(data)
    required={'schema','status','base_ingester_sha256','additional_roles','reviewer','reason','second_read_evidence'}
    m.require(isinstance(registry,dict)and set(registry)==required,'Campos del registro de roles incompatibles')
    m.require(registry['schema']==REGISTRY_SCHEMA and registry['status']=='APPROVED_AFTER_SECOND_READ',
              'Registro v2 sin aprobación y segunda lectura')
    m.require(registry['base_ingester_sha256']==BASE_INGESTER_SHA256,'Otra procedencia de ingester')
    roles=registry['additional_roles']
    m.require(isinstance(roles,list)and all(isinstance(x,str)and
              re.fullmatch('[a-z][a-z0-9_]{0,63}',x)for x in roles)and len(roles)==len(set(roles)),'Lista explícita de roles inválida')
    m.require(frozenset(roles)==APPROVED_ADDITIONAL_ROLES and not(set(roles)&BASE_ROLES),
              'Los roles nuevos no coinciden con la lista finita fijada')
    m.require('honorific_suffix'not in roles,'La gramática de sufijo honorífico está expresamente aplazada')
    m.require(all(isinstance(registry[k],str)and registry[k].strip()for k in('reviewer','reason')),
              'Falta responsable o motivo del registro')
    evidence=registry['second_read_evidence']
    m.require(isinstance(evidence,list)and evidence,'Falta evidencia de segunda lectura')
    for item in evidence:
        m.require(isinstance(item,dict)and set(item)=={'path','sha256'},'Evidencia de segunda lectura inválida')
        rel=Path(item['path'])
        m.require(not rel.is_absolute()and '..'not in rel.parts,'Ruta de evidencia fuera del proyecto')
        project=MEMORY_DIR.parents[2]
        m.require((project/rel).is_file()and _file_sha(project/rel)==item['sha256'],'Segunda lectura no coincide')
    return registry


def marker_mapping(document):
    # Only an independently validated, hash-pinned raw-atom lexer may replace v1.
    if MARKER_LEXER_RELATIVE_PATH is None:return _marker_mapping_v1(document)
    m.require(isinstance(APPROVED_MARKER_LEXER_SHA256,str)and
              re.fullmatch('[0-9a-f]{64}',APPROVED_MARKER_LEXER_SHA256),'Lexer v2 no fijado')
    rel=Path(MARKER_LEXER_RELATIVE_PATH)
    m.require(not rel.is_absolute()and '..'not in rel.parts,'Ruta de lexer inválida')
    path=MEMORY_DIR/rel
    m.require(_file_sha(path)==APPROVED_MARKER_LEXER_SHA256,'Lexer de marcadores distinto del validado')
    proof_path=MEMORY_DIR/MARKER_LEXER_PROOF_RELATIVE_PATH
    m.require(_file_sha(proof_path)==APPROVED_MARKER_LEXER_PROOF_SHA256,'Prueba del lexer distinta de la validada')
    proof=json.loads(proof_path.read_text())
    m.require(proof['passed']and proof['mapper']['sha256']==APPROVED_MARKER_LEXER_SHA256 and
              proof['frozen_v1']['sha256']==BASE_INGESTER_SHA256 and not proof['frozen_v1']['modified'],
              'Contrato del lexer v2 no acreditado')
    spec=importlib.util.spec_from_file_location('_kotogba_b10_marker_lexer_private',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    mapping=getattr(module,MARKER_LEXER_EXPORT)(document)
    m.require(isinstance(mapping,dict),'Correspondencia de lexer inválida')
    raw=document['raw_source'];jp=document['japanese'].encode();last_raw=-1;last_display=-1
    for (lo,hi),(start,end,visible)in sorted(mapping.items()):
        m.require(all(type(n)is int for n in(lo,hi,start,end))and 0<=lo<hi<=len(jp)and
                  0<=start<end<=len(raw)and end==start+2,'Límites del lexer inválidos')
        token=re.fullmatch(r'\{(FD|F7):([0-9A-F]{2})\}',visible)
        m.require(token and raw[start:end]==bytes(({'FD':253,'F7':247}[token[1]],int(token[2],16)))and
                  jp[lo:hi].decode()==visible,'Identidad raw/display del lexer divergente')
        m.require(start>last_raw and lo>last_display,'Orden de marcadores del lexer ambiguo')
        last_raw=start;last_display=lo
    return mapping


def ingest(db,packet_path,*,registry_path,fault_hook=None,expected_packet_sha256=None):
    packet_bytes=Path(packet_path).read_bytes()
    if expected_packet_sha256 is not None:
        m.require(hashlib.sha256(packet_bytes).hexdigest()==expected_packet_sha256,'Paquete distinto del revisado')
    packet=json.loads(packet_bytes)
    _approved_registry(registry_path)

    m.require(isinstance(packet,dict) and set(packet) in (PACKET_KEYS,PACKET_KEYS|{'packet_id'}),
              'Campos de paquete de variables incompatibles')
    m.require(packet['schema']==SCHEMA and isinstance(packet['game_id'],str),'Esquema de variables incompatible')
    contract=packet['contract']
    m.require(isinstance(contract,dict)and set(contract)==CONTRACT_KEYS,'Contrato de versión v2 incompatible')
    m.require(isinstance(contract['database_id'],str)and contract['database_id'].startswith('memory:'),'Falta identidad de memoria')
    m.require(all(type(contract[k])is int and contract[k]>=0 for k in('expected_memory_revision','expected_runtime_revision')),
              'Revisiones esperadas de v2 inválidas')
    m.require(contract['role_registry_sha256']==APPROVED_ROLE_REGISTRY_SHA256,'Contrato de otro registro de roles')
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
            m.require(m.meta(c,'database_id')==contract['database_id'],'Paquete para otra memoria')
            m.require(g['revision']==contract['expected_memory_revision']and
                      g['runtime_revision']==contract['expected_runtime_revision'],
                      'Contrato de revisión v2 obsoleto; volver a comprobar todas las preimágenes')
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
            report={'schema':'kotogba.variable_link_import/2','packet_id':pid,'packet_sha256':ph,
                    'game_id':game,'revision':revision,'added_links':len(prepared),'changed_documents':len(docs),
                    'roles':dict(roles),'backup':backup,'already_applied':False,
                    'packet_schema':SCHEMA,'role_registry_sha256':APPROVED_ROLE_REGISTRY_SHA256,
                    'base_ingester_sha256':BASE_INGESTER_SHA256,'version_contract':contract,
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
    p.add_argument('--packet',type=Path,required=True);p.add_argument('--report',type=Path);p.add_argument('--registry',type=Path,required=True);p.add_argument('--packet-sha256');a=p.parse_args()
    result=ingest(a.db,a.packet,registry_path=a.registry,expected_packet_sha256=a.packet_sha256)
    if a.report:m.write_json(a.report,result)
    print(json.dumps(result,ensure_ascii=False,indent=2))
