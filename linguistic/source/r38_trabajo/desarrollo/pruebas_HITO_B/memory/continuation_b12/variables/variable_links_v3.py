#!/usr/bin/env python3
"""B12 v3: finite new semantic roles, explicit registry and revision contract.

Derived from frozen B10 variable_links_v2.py, whose source and globals remain
unchanged. Only its validated raw/display marker mapper is reused. The writer
and validation code below belong to v3. No grammar/composite/transport role is
accepted. A proposed registry is rejected until independent review is recorded.
"""
from collections import Counter
from pathlib import Path
import argparse
import fcntl
import hashlib
import importlib.util
import json
import re
import sys

sys.dont_write_bytecode=True
MEMORY_DIR=Path(__file__).resolve().parents[2]
if str(MEMORY_DIR) not in sys.path:sys.path.insert(0,str(MEMORY_DIR))
import memory as m

SCHEMA='kotogba.variable_links/3'
REGISTRY_SCHEMA='kotogba.variable_link_roles/3'
SOURCE_V2_SHA256='fc279d71adeda1276c4fa5baf34e8e133ea80fb4d801044aaf8a28a559137b80'
ROLES=frozenset({'address_term','elapsed_centiseconds','safari_steps_remaining',
    'safari_steps_limit','percentage_integer_part','percentage_fractional_part',
    'presses_per_second_integer_part','presses_per_second_fractional_part'})
CANDIDATES=frozenset({2,6,96,97,98,278,279,447,448,452,453,454})
CONTRACT_KEYS={'database_id','expected_memory_revision','expected_runtime_revision','role_registry_sha256'}
ITEM_KEYS={'source_offset','raw_start','raw_end','display_byte_start','display_byte_end','visible',
    'before','raw_source_sha256','replacement','role','reason','provenance'}
PACKET_KEYS={'schema','game_id','base_sha256','reviewer','reason','provenance','items','contract'}
REGISTRY_KEYS={'schema','status','source_v2_sha256','roles','role_definitions',
    'approved_candidate_indices','semantic_items_sha256','reviewer','reason','second_read_evidence'}

def file_sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def semantic_digest(items):
    # Contract and top-level provenance can be rebased by the sole writer;
    # marker identities, source hashes and authored card bodies cannot drift.
    reduced=[]
    for item in items:
        row={key:item[key] for key in sorted(ITEM_KEYS-{'provenance'})}
        row['candidate_index']=item['provenance']['candidate_index']
        reduced.append(row)
    reduced.sort(key=lambda x:x['candidate_index'])
    return hashlib.sha256(json.dumps(reduced,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def frozen_mapper():
    path=MEMORY_DIR/'continuation_b10/variable_links_v2.py'
    m.require(file_sha(path)==SOURCE_V2_SHA256,'La fuente v2 histórica ha cambiado')
    spec=importlib.util.spec_from_file_location('_b12_frozen_v2_mapper',path)
    v2=importlib.util.module_from_spec(spec);spec.loader.exec_module(v2)
    # This verifies the unchanged v2 chain of authority; no global is assigned.
    v2._approved_registry(MEMORY_DIR/'continuation_b10/assembly/ROLE_REGISTRY_B10_v2.json')
    return v2.marker_mapping

def approved_registry(path,expected_sha256):
    m.require(isinstance(expected_sha256,str) and re.fullmatch('[a-f0-9]{64}',expected_sha256),
              'Falta el hash explícito del registro revisado')
    m.require(file_sha(path)==expected_sha256,'Registro v3 distinto del revisado')
    data=json.loads(Path(path).read_text())
    m.require(isinstance(data,dict) and set(data)==REGISTRY_KEYS,'Campos de registro v3 incompatibles')
    m.require(data['schema']==REGISTRY_SCHEMA and data['status']=='APPROVED_AFTER_SECOND_READ',
              'Registro v3 propuesto o sin segunda lectura: no se importa')
    m.require(data['source_v2_sha256']==SOURCE_V2_SHA256,'Registro v3 de otra derivación')
    roles=data['roles']
    m.require(isinstance(roles,list) and len(roles)==len(set(roles)) and frozenset(roles)==ROLES,
              'V3 sólo admite los ocho roles semánticos explícitos')
    defs=data['role_definitions']
    m.require(isinstance(defs,dict) and set(defs)==ROLES,'Faltan definiciones de roles v3')
    for role,definition in defs.items():
        m.require(definition['stable_identity']==f'kotogba.semantic_role/{role}/1' and definition['type']==4,
                  'Identidad estable o tipo del rol divergente')
        m.require(all(isinstance(definition[k],str) and definition[k].strip()
                      for k in ('meaning_es','explanation_es','value_class')),'Definición semántica vacía')
    ids=data['approved_candidate_indices']
    m.require(isinstance(ids,list) and len(ids)==len(CANDIDATES) and frozenset(ids)==CANDIDATES,
              'El registro no corresponde a los doce marcadores revisados')
    m.require(re.fullmatch('[a-f0-9]{64}',data['semantic_items_sha256']) is not None,'Falta sello de los doce ítems')
    m.require(all(isinstance(data[k],str) and data[k].strip() for k in ('reviewer','reason')),
              'Falta responsable o motivo de la segunda lectura')
    proofs=data['second_read_evidence']
    m.require(isinstance(proofs,list) and proofs,'Falta evidencia de segunda lectura v3')
    project=MEMORY_DIR.parents[2]
    for proof in proofs:
        m.require(isinstance(proof,dict) and set(proof)=={'path','sha256'},'Prueba de revisión v3 inválida')
        rel=Path(proof['path'])
        m.require(not rel.is_absolute() and '..' not in rel.parts,'Prueba de revisión fuera del proyecto')
        f=project/rel
        m.require(f.is_file() and file_sha(f)==proof['sha256'],'La prueba de revisión no coincide')
    return data

def prepare(c,packet,registry,registry_sha256):
    """Validate a packet against a transaction snapshot; no mutation."""
    m.require(isinstance(packet,dict) and set(packet) in (PACKET_KEYS,PACKET_KEYS|{'packet_id'}),
              'Campos de paquete v3 incompatibles')
    m.require(packet['schema']==SCHEMA and packet['game_id']=='bprj1','Esquema/juego v3 incompatible')
    contract=packet['contract']
    m.require(isinstance(contract,dict) and set(contract)==CONTRACT_KEYS,'Contrato v3 incompatible')
    m.require(contract['role_registry_sha256']==registry_sha256,'Contrato de otro registro v3')
    m.require(all(type(contract[k]) is int and contract[k]>=0
                  for k in ('expected_memory_revision','expected_runtime_revision')),'Revisiones v3 inválidas')
    m.require(isinstance(packet['items'],list) and len(packet['items'])==12,'V3 exige los doce ítems revisados')
    m.require(semantic_digest(packet['items'])==registry['semantic_items_sha256'],
              'Cambió el contenido o la identidad de los doce ítems revisados')
    for key in ('reviewer','reason'):
        m.require(isinstance(packet[key],str) and packet[key].strip(),'Falta revisión contextual')
    m.require(isinstance(packet['provenance'],dict) and packet['provenance'],'Falta procedencia')
    g=c.execute('SELECT * FROM koto_games WHERE game_id=?',(packet['game_id'],)).fetchone()
    m.require(g and g['base_sha256']==packet['base_sha256'],'Otra base')
    m.require(m.meta(c,'database_id')==contract['database_id'],'Otra memoria')
    m.require(g['revision']==contract['expected_memory_revision'] and
              g['runtime_revision']==contract['expected_runtime_revision'],
              'Contrato v3 obsoleto: revalidar todas las preimágenes')
    mapper=frozen_mapper();docs={};maps={};seen=set();seen_candidates=set();prepared=[];roles=Counter()
    for item in packet['items']:
        m.require(isinstance(item,dict) and set(item)==ITEM_KEYS,'Campos de ítem v3 incompatibles')
        candidate=item['provenance'].get('candidate_index')
        m.require(candidate in CANDIDATES and candidate not in seen_candidates,'Ítem fuera de alcance o repetido')
        seen_candidates.add(candidate)
        m.require(type(item['source_offset']) is int and item['source_offset']>=0,'Fuente inválida')
        did=m.document_id(packet['game_id'],item['source_offset'])
        if did not in docs:
            docs[did]=c.execute('SELECT * FROM koto_documents WHERE document_id=?',(did,)).fetchone()
            m.require(docs[did] is not None,'Documento desconocido');maps[did]=mapper(docs[did])
        doc=docs[did];lo,hi=item['display_byte_start'],item['display_byte_end']
        m.require(type(lo) is int and type(hi) is int and 0<=lo<hi<=len(doc['japanese'].encode()),'Rango inválido')
        m.require(type(item['raw_start']) is int and type(item['raw_end']) is int,'Rango fuente inválido')
        m.require(m.digest(doc['raw_source'])==item['raw_source_sha256'],'Fuente binaria distinta')
        m.require(maps[did].get((lo,hi))==(item['raw_start'],item['raw_end'],item['visible']),'Marcador no exacto')
        m.require(doc['japanese'].encode()[lo:hi].decode()==item['visible'],'Superficie distinta')
        m.require(item['role'] in ROLES,'Rol fuera de registro v3')
        m.require(isinstance(item['reason'],str) and item['reason'].strip() and
                  isinstance(item['provenance'],dict) and item['provenance'],'Sin revisión del ítem')
        replacement=item['replacement']
        m.require(isinstance(replacement,dict) and set(replacement)==m.REPLACEMENT_KEYS,'Ficha incompatible')
        m.require(type(replacement['type']) is int and replacement['type']==4 and
                  replacement['surface']==item['visible'] and replacement['reading']=='',
                  'No se permite fijar el nombre, número o lectura variable')
        for key in ('meaning_es','explanation_es'):
            m.require(isinstance(replacement[key],str) and replacement[key].strip() and
                      '\0' not in replacement[key],'Ficha contextual inválida')
        before=[s for s in m._active_spans(c,did) if s['display_byte_start']<hi and lo<s['display_byte_end']]
        m.require(item['before']==before==[],'Ya existe un enlace: no se sobrescribe')
        oid=m.occurrence_id(did,lo,hi)
        m.require(oid not in seen,'Identidad duplicada');seen.add(oid)
        m.require(c.execute('SELECT 1 FROM koto_occurrences WHERE occurrence_id=?',(oid,)).fetchone() is None,
                  'Existe una identidad histórica en esa posición')
        units=c.execute('SELECT * FROM koto_units WHERE document_id=? AND display_start<=? AND display_end>=? AND raw_start<=? AND raw_end>=?',
                        (did,lo,hi,item['raw_start'],item['raw_end'])).fetchall()
        m.require(units,'Marcador sin unidad original')
        prepared.append((did,oid,item));roles[item['role']]+=1
    m.require(seen_candidates==CANDIDATES,'Falta un marcador revisado')
    return g,docs,prepared,roles

def ingest(db,packet_path,*,registry_path,approved_registry_sha256,expected_packet_sha256,fault_hook=None):
    """Single-writer transaction. Caller must supply both reviewed file hashes."""
    m.require(file_sha(packet_path)==expected_packet_sha256,'Paquete distinto del revisado')
    registry=approved_registry(registry_path,approved_registry_sha256)
    packet=json.loads(Path(packet_path).read_text());ph=m.digest(packet)
    pid=packet.get('packet_id','variable_links_v3:'+ph)
    m.require(isinstance(pid,str) and pid.strip(),'Identidad de paquete inválida')
    db=Path(db)
    with db.with_name(db.name+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);c=m.connect(db)
        try:
            m.check_schema(c);c.execute('BEGIN IMMEDIATE')
            prior=m._existing_import(c,pid,ph)
            if prior:c.rollback();return prior
            g,docs,prepared,roles=prepare(c,packet,registry,approved_registry_sha256)
            backup=m._backup(c,db,ph);revision=g['revision']+1;game=packet['game_id']
            report={'schema':'kotogba.variable_link_import/3','packet_id':pid,'packet_sha256':ph,
                    'game_id':game,'revision':revision,'added_links':len(prepared),'changed_documents':len(docs),
                    'roles':dict(roles),'backup':backup,'already_applied':False,'packet_schema':SCHEMA,
                    'role_registry_sha256':approved_registry_sha256,'source_v2_sha256':SOURCE_V2_SHA256,
                    'version_contract':packet['contract'],'existing_links_modified':0,
                    'stored_personal_values':False,'source_units_modified':False}
            c.execute('INSERT INTO koto_imports VALUES(?,?,?,?)',(pid,ph,game,m.canonical(report)))
            m._store_packet(c,pid,packet)
            for n,(did,oid,item) in enumerate(prepared,1):
                card=m._new_card(c,item['replacement'])
                provenance={'packet_id':pid,'reviewer':packet['reviewer'],'role':item['role'],
                    'reason':item['reason'],'source_offset':item['source_offset'],'evidence':item['provenance'],
                    'value_policy':'runtime_frozen_visible_value_only'}
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
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--db',type=Path,required=True);p.add_argument('--packet',type=Path,required=True)
    p.add_argument('--registry',type=Path,required=True);p.add_argument('--approved-registry-sha256',required=True)
    p.add_argument('--packet-sha256',required=True);p.add_argument('--report',type=Path)
    a=p.parse_args()
    result=ingest(a.db,a.packet,registry_path=a.registry,
                  approved_registry_sha256=a.approved_registry_sha256,expected_packet_sha256=a.packet_sha256)
    if a.report:m.write_json(a.report,result)
    print(json.dumps(result,ensure_ascii=False,indent=2))
