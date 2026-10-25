#!/usr/bin/env python3
"""B11-only versioned exporter: preserve the finite B10 header dispatch repair.

The historical memory/runtime_cards modules are immutable. Pair staging and
commit-last publication below are copied from the pinned v1 implementation.
"""
from pathlib import Path
import argparse,copy,hashlib,json,os,struct,sys
HERE=Path(__file__).resolve().parent;MEM=HERE.parent
if str(MEM)not in sys.path:sys.path.insert(0,str(MEM))
import memory as m
# Explicit aliases only. No mutation of m, its globals, or runtime_cards.
require=m.require;connect=m.connect;check_schema=m.check_schema;kl=m.kl;digest=m.digest
kc15_companion=m.kc15_companion;SCHEMA=m.SCHEMA;file_sha=m.file_sha;meta=m.meta;write_json=m.write_json
B10_EXPORTER_SHA256='538ff023b4922a172a9f146b71e3df6d6750100542725e28eb54f96aee132389'
DATABASE_ID='memory:27d276afcdc1257ae9cb8448a66b1cddba8bcc68d3adf89eb0ba0a12d3dc21c9'
RUNTIME_BASE_SHA256='d3fbe3d59d3dd65dc0c9f6436fcd2c1851df375a74aba9a92c84f5a4571542d9'
B11_REVIEW_PACKET_ID='review:B11-first100-independent-second-read-approved:20261025'
B11_REVIEW_FILE_SHA256='07e4221c81ac2f67dd016f7fea37d1ad3f7d5b6eac97c306444af2541ea1d2a6'
B11_REVIEW_CANONICAL_SHA256='83a12a0f99af5fce074c0ee425efe218b24fe8a1698235177e189cce0b1f389a'
B11_PATCH_PACKET_ID='patches:B11-v2-reviewed-union-exact-raw:20261025'
B11_PATCH_FILE_SHA256='9580968de1e32981f9ed3bf93cfaaac9f19ab5031efa6f5d67e395233fc50b3a'
B11_EXACT_UNION_ADAPTER_SHA256='9f71c4c8017f96068cee6ea562d0047510e860674fb1020c30a5bc00734e74b8'
B11_V1_PATCH_PACKET_ID='patches:B11-reviewed100-union-reprepared-runtime26:20261025'
B11_V1_REPREPARED_PATCH_CANONICAL_SHA256='5a465a421b826fa5b2131671d2e058ba1ebb354d3d3ea8ed86b2354f85c645cc'
B11_PATCH_CANONICAL_SHA256='a80935e911da8fda7708eaed014de613aa868943d5eaa29adeae3375b9b65fe4'
B11_ORIGINAL_PATCH_FILE_SHA256='9a288be133fd720449d0ce984992b6fa8b5c13a8769e910036ebed961783afcf'
B11_ORIGINAL_PATCH_CANONICAL_SHA256='55c2fb51c4ee8947bf3c6f96c771d02146d7710ac2fda940c0402d92364c049b'
HISTORICAL_MEMORY_SHA256='ed5337c45884b852446be7c195e2ed8ae7b9f305e4dd3a4e67a2a6dbb603ae99'
HISTORICAL_RUNTIME_CARDS_SHA256='d1876d6358506d89e5ff59d3db4fd23f92d659a48b84a2683eda9dbc75241e92'
PACKET_SHA256='880ed6a82fe9d5948427cfcc9efb5b481bc8437ae98b3a2e1f2e4b43e5ac1be2'
REGISTRY_SHA256='ec23ce8c8819b9ce9f78680def6d065a85959dfe442f9824c8107345979b0aa9'
APPROVED_PACKET_ID='variable_links:B10-v2-reviewed388:af44b41a68617d27653e81176ff65db0d00d10efbab38f0add86489f51adf8b0'
APPROVED_PACKET_CANONICAL_SHA256='6d3074d215643ed9994c7d97e574ce6a55d45dad47ca05cb3bb3fa656f93036c'
REPAIR_SOURCE=0x3D9703
REPAIR_PREIMAGE_SHA256='608f901d5789ab4512a397773522843b30b0d628c921ea159a5bb74a3e683bfb'
INITIAL_REJECTED_KL29_SHA256='72f41f57dfa54be7f799a45850a1c956597caeb8c5300d2eefe0bb41abd3f8d6'
CONSUMER_HEADER_REFERENCE={'path':'desarrollo/fuentes/code/core/arm9/source/Application/KotoGbaLinguistic.inc::LinguisticDocumentHeader','reviewed_source_sha256':'943681dbecec41417eb0406ab78c1779befae4ec38d05e52b16579b6d95ba561','maximum_units':192,'maximum_notes':6}


def _range(at,size,limit):return 0<=at<=limit and 0<=size<=limit-at

def compiled_header(document,loading_start,loading_end):
    """Raw-byte equivalent of the consumer header gate, without codec recovery."""
    doc=bytes(document);size=len(doc)
    require(size>=96 and doc[:8]==b'KOTOD029','Invalid KOTOD029 header')
    u32=lambda at:struct.unpack_from('<I',doc,at)[0]
    u16=lambda at:struct.unpack_from('<H',doc,at)[0]
    require(u32(8)==loading_start and u32(12)==size,'Source/byte length differs from loading contract')
    units,nunits,notes,nnotes,strings,raw,fragments,nfragments,spanish,nspanish,flags=[u32(at)for at in(20,24,28,32,36,40,44,48,52,56,60)]
    def text(at,n):return(at==0 and n==0)or(at>=strings and _range(at,n+1,size)and doc[at+n]==0 and b'\0'not in doc[at:at+n])
    require(raw>0 and raw<=512 and raw==loading_end-loading_start,'Invalid raw-source length')
    require(nunits<=192 and nnotes<=6 and nfragments<=32,'Consumer table count limit exceeded')
    require(units>=96+raw and _range(units,nunits*12,size),'Invalid unit table range')
    require(notes>=units+nunits*12 and _range(notes,nnotes*32,size),'Invalid note table range')
    require(strings>=notes+nnotes*32 and strings<=size,'Invalid strings table range')
    require(not nfragments or(_range(fragments,nfragments*16,strings)and fragments>=notes+nnotes*32),'Invalid fragments range')
    require(not(flags&0xff0000f8)and text(spanish,nspanish),'Invalid reserved flags or Spanish text range')
    words=(flags>>8)&65535;nwords=0;inline=0;nspans=0;ncards=0
    if flags&1:
        previous=fragments+nfragments*16 if nfragments else notes+nnotes*32
        require(words>=previous and _range(words,4,strings),'Invalid word table range')
        nwords=u16(words);require(u16(words+2)==0 and nwords<=256 and _range(words+4,nwords*8,strings),'Invalid word table count/reserved')
    else:require(words==0,'Word offset without dispatch flag')
    if flags&4:
        require(flags&1,'Inline dispatch without word dispatch')
        inline=words+4+nwords*8;require(_range(inline,16,strings),'Invalid inline header range')
        nspans=u16(inline+8);ncards=u16(inline+10)
        require(doc[inline:inline+8]==b'KOTOV032'and 0<nspans<=512 and 0<ncards<=256 and u16(inline+12)==u16(inline+14)==0,'Invalid inline magic/count/reserved')
        require(_range(inline+16,nspans*12+ncards*24,strings),'Invalid inline tables range')
    physical_inline=words+4+nwords*8 if flags&1 else None;physical_spans=physical_cards=0
    if physical_inline is not None and _range(physical_inline,16,strings)and doc[physical_inline:physical_inline+8]==b'KOTOV032':
        physical_spans=u16(physical_inline+8);physical_cards=u16(physical_inline+10)
    return {'passed':True,'flags':flags,'word_flag':bool(flags&1),'inline_dispatch':bool(flags&4),'words_at':words,'word_count':nwords,'inline_at':inline,'inline_spans':nspans,'inline_cards':ncards,'physical_inline_at':physical_inline,'physical_spans':physical_spans,'physical_cards':physical_cards,'strings_at':strings}


def publishable_header(document,loading_start,loading_end):
    result=compiled_header(document,loading_start,loading_end)
    require(not(result['physical_spans']and result['physical_cards'])or result['inline_dispatch'],
            'Nonempty physical inline table has no compiled dispatch bit4')
    return result


def repair_emitted_document(document,source,source_end):
    check=compiled_header(document,source,source_end);fix=None
    if check['physical_spans']and check['physical_cards']and not check['inline_dispatch']:
        require(source==REPAIR_SOURCE and digest(document)==REPAIR_PREIMAGE_SHA256 and check['flags']==0x7003 and
                check['word_flag']and(check['words_at'],check['word_count'],check['physical_inline_at'],check['physical_spans'],check['physical_cards'],check['strings_at'])==(112,0,116,1,1,168),
                'Unexpected missing inline flag or changed B10 preimage; refuse general repair')
        out=bytearray(document);struct.pack_into('<I',out,60,check['flags']|4);fixed=bytes(out)
        require(fixed[:60]==document[:60]and fixed[64:]==document[64:],'Header repair altered body/payload')
        fix={'source_offset':source,'before_sha256':digest(document),'after_sha256':digest(fixed),'before_flags':check['flags'],'after_flags':check['flags']|4,'byte_offset':60,'changed_bit':4,'body_and_card_identities_unchanged':True}
        document=fixed
    publishable_header(document,source,source_end)
    return document,fix


def _runtime_document(conn,d,capacity,metadata=None):
    document,source_meta,detail=m._runtime_document(conn,d,capacity,metadata)
    document,fix=repair_emitted_document(document,d['source_offset'],d['source_end'])
    detail=dict(detail);detail['v3_header_fix']=fix
    return document,source_meta,detail


def _stored_packet(conn,pid,sha,label):
    stored=conn.execute('SELECT packet_json FROM koto_packets WHERE packet_id=?',(pid,)).fetchone()
    require(stored is not None,f'{label} packet is absent from complete memory history')
    packet=json.loads(stored[0])
    require(digest(packet)==sha and packet.get('packet_id')==pid,f'Wrong {label} stored packet identity')
    adopted=m._existing_import(conn,pid,sha)
    require(adopted is not None and adopted.get('packet_id')==pid and adopted.get('packet_sha256')==sha and
            adopted.get('game_id')=='bprj1',f'{label} packet is not adopted for this game')
    return packet,adopted


def _contract(conn,g):
    # B10's whole exporter is an immutable derivation authority, never patched
    # or called after changing its globals/revision guard.
    require(file_sha(MEM/'continuation_b10/export_runtime_v2.py')==B10_EXPORTER_SHA256,
            'Pinned B10 versioned exporter source changed')
    require(file_sha(MEM/'memory.py')==HISTORICAL_MEMORY_SHA256 and file_sha(MEM/'runtime_cards.py')==HISTORICAL_RUNTIME_CARDS_SHA256,'Historical exporter source changed')
    require(file_sha(HERE/'patch_import_v2.py')==B11_EXACT_UNION_ADAPTER_SHA256,'Pinned B11 exact-union importer source changed')
    require((g['revision'],g['runtime_revision'])==(39,27),'Versioned exporter is restricted to B11 memory39/runtime27')
    require(g['game_id']=='bprj1' and g['base_sha256']==RUNTIME_BASE_SHA256,'Other B11 runtime base/game')
    require(meta(conn,'database_id')==DATABASE_ID,'Other canonical memory identity')
    packet,adopted=_stored_packet(conn,APPROVED_PACKET_ID,APPROVED_PACKET_CANONICAL_SHA256,'Approved B10')
    require(packet['schema']=='kotogba.variable_links/2'and len(packet['items'])==388,'Wrong B10 semantic adoption')
    require(packet['contract']['role_registry_sha256']==REGISTRY_SHA256,'Other approved finite role registry')
    require(adopted['added_links']==388 and adopted['revision']==37 and adopted['role_registry_sha256']==REGISTRY_SHA256,'B10 reviewed packet is not adopted')
    review,review_adopted=_stored_packet(conn,B11_REVIEW_PACKET_ID,B11_REVIEW_CANONICAL_SHA256,'B11 review100')
    require(review['schema']=='kotogba.review/1' and review['schema_version']==1 and review['content_language']=='es' and
            len(review['items'])==100 and review['base']=={'database_id':DATABASE_ID,'game_id':'bprj1','runtime_base_sha256':RUNTIME_BASE_SHA256,'revision':36},
            'Wrong B11 reviewed100 schema/count/base provenance')
    counts={decision:sum(item['review']['decision']==decision for item in review['items']) for decision in ('keep','replace','reject')}
    require(counts=={'keep':74,'replace':24,'reject':2},'Wrong B11 reviewed100 decision inventory')
    require(review_adopted.get('schema')=='kotogba.review_import/1' and review_adopted.get('revision')==38 and review_adopted.get('items')==100,
            'B11 reviewed100 is not adopted at memory38 before its union')
    patch,patch_adopted=_stored_packet(conn,B11_PATCH_PACKET_ID,B11_PATCH_CANONICAL_SHA256,'B11 exact union1')
    require(patch['schema']=='kotogba.exact_union_import/2' and patch['game_id']=='bprj1' and patch['base_sha256']==RUNTIME_BASE_SHA256 and
            patch['expected_memory_revision']==38 and patch['expected_memory_runtime_revision']==26 and len(patch['items'])==1,
            'Wrong B11 exact union schema/count/dependent38/26')
    union=patch['items'][0];decision=union['decision']
    require((union['raw_start'],union['raw_end'])==(48,52) and
            (decision['source_offset'],decision['start'],decision['end'],decision['visible'])==(0x168C11,126,138,'て　たら') and
            patch['review_binding']=={'packet_id':B11_REVIEW_PACKET_ID,'packet_sha256':B11_REVIEW_CANONICAL_SHA256,
                'file_sha256':B11_REVIEW_FILE_SHA256,'adopted_memory_revision':38},
            'Wrong B11 exact source/display union or review dependency')
    require(patch_adopted.get('schema')=='kotogba.patch_import/2' and patch_adopted.get('revision')==39 and
            patch_adopted.get('runtime_revision')==27 and patch_adopted.get('patches')==1 and
            patch_adopted.get('exact_raw_range')==[48,52] and patch_adopted.get('exact_display_range')==[126,138] and
            patch_adopted.get('adjacent_source_span_preserved')==[45,48] and
            patch_adopted.get('review_dependency_sha256')==B11_REVIEW_CANONICAL_SHA256 and
            patch_adopted.get('adapter_sha256')==B11_EXACT_UNION_ADAPTER_SHA256 and
            patch_adopted.get('existing_source_units_modified') is False,
            'B11 exact union is not adopted at39/27 with the finite raw48:52 contract')
    original_patch_id='patches:B11-first100-one-auxiliary-union:d4f44355636a5c1ebc5d41215a4c8a50e7a738e36f0b2fb10a3f5c4600cab667'
    require(conn.execute('SELECT 1 FROM koto_imports WHERE packet_id IN (?,?)',
            (original_patch_id,B11_V1_PATCH_PACKET_ID)).fetchone() is None,'Superseded B11 v1 union must remain unimported')
    return {'memory_revision':39,'runtime_revision':27,'historical_B10':{
        'reviewed_packet_file_sha256_reference':PACKET_SHA256,'reviewed_packet_canonical_sha256':APPROVED_PACKET_CANONICAL_SHA256,
        'role_registry_sha256':REGISTRY_SHA256,'adopted_revision':37,'adapter_sha256':B10_EXPORTER_SHA256},
        'B11_operations_in_order':[
            {'packet_id':B11_REVIEW_PACKET_ID,'canonical_sha256':B11_REVIEW_CANONICAL_SHA256,
             'original_file_sha256_reference':B11_REVIEW_FILE_SHA256,'schema':review['schema'],'items':100,
             'decisions':counts,'adopted_revision':38,'original_base_revision_preserved':36},
            {'packet_id':B11_PATCH_PACKET_ID,'canonical_sha256':B11_PATCH_CANONICAL_SHA256,
             'original_file_sha256_reference':B11_ORIGINAL_PATCH_FILE_SHA256,
             'original_canonical_sha256_reference':B11_ORIGINAL_PATCH_CANONICAL_SHA256,
             'schema':patch['schema'],'items':1,'expected_memory_revision':38,'expected_memory_runtime_revision':26,
             'adopted_revision':39,'adopted_runtime_revision':27,'exact_raw_range':[48,52],'exact_display_range':[126,138],
             'adapter_sha256':B11_EXACT_UNION_ADAPTER_SHA256,'packet_file_sha256_reference':B11_PATCH_FILE_SHA256,
             'superseded_v1_reprepared_digest_not_adopted':B11_V1_REPREPARED_PATCH_CANONICAL_SHA256}],
        'historical_memory_sha256':HISTORICAL_MEMORY_SHA256,'historical_runtime_cards_sha256':HISTORICAL_RUNTIME_CARDS_SHA256,
        'consumer_header_reference':CONSUMER_HEADER_REFERENCE}


def export_runtime(db_path, output, game='bprj1', capacity=None, base_kc15=None, output_kc15=None, fault_hook=None):
    """Export KL29; pass both KC15 paths to publish a validated SD pair.

    A KL29-only projection with revised Spanish is explicitly incomplete for
    installation: the compiled reader obtains its translation from KC15.
    """
    output=Path(output)
    require(output.resolve()!=Path(db_path).resolve(),'El paquete no puede reemplazar la memoria')
    require((base_kc15 is None)==(output_kc15 is None),'Se requieren ambas rutas KC15 para exportar la pareja')
    companion=None;companion_report=None
    if base_kc15 is not None:
        base_kc15=Path(base_kc15);output_kc15=Path(output_kc15)
        require(output.parent.resolve()==output_kc15.parent.resolve(),'La pareja debe publicarse en una misma carpeta')
        require(len({output.resolve(),output_kc15.resolve(),base_kc15.resolve(),Path(db_path).resolve()})==4,
                'Las salidas no pueden reemplazar la memoria ni el compañero base')
    conn=connect(db_path,readonly=True)
    try:
        check_schema(conn);conn.execute('BEGIN')
        g=conn.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
        require(g is not None,'Juego desconocido')
        contract=_contract(conn,g);header_fixes=[];header_documents=0
        base=g['base_payload'];h,entries=kl.package(base);h=list(h)
        require(digest(base)==g['base_sha256'],'Base KL29 alterada')
        if base_kc15 is not None:
            companion,companion_report=kc15_companion.export_bytes(conn,base_kc15,game)
            h[11]=companion_report['output_checksum']
        capacity=g['capacity'] if capacity is None else capacity
        require(type(capacity)is int and 512<=capacity<=65024 and capacity%512==0,'Capacidad de sector inválida')
        provenance=json.loads(base[h[13]*512:h[13]*512+h[14]])
        metas={int(s['id'].split('_')[-1],16):s for s in provenance['sources']}
        changed=[];active_count=0
        for d in conn.execute('SELECT * FROM koto_documents WHERE game_id=? ORDER BY source_offset',(game,)):
            off=d['source_offset'];old=d['original_bytes']
            require(entries[off]['doc']==old and entries[off]['end']==d['source_end'],'Documento fuera de la base declarada')
            new,sm,detail=_runtime_document(conn,d,capacity,metas.get(off))
            header_documents+=1
            if detail['v3_header_fix']:header_fixes.append(detail['v3_header_fix'])
            active_count+=detail['active_spans']
            if new==old:
                continue
            if off in metas:metas[off].clear();metas[off].update(sm)
            entries[off]['doc']=new
            item={'source_offset':off,'source_id':d['document_id'],'before_sha256':digest(old),
                  'after_sha256':digest(new),'bytes':len(new),
                  **{key:detail[key] for key in ('compacted','active_spans','historical_cards_retained','stable_catalog_cards')}}
            # No new provenance keys on untouched historical exports: B04 must
            # remain byte reproducible when no translation revision is present.
            if detail['translation_revision']:
                item['translation_review']={key:detail[key] for key in
                    ('translation_revision','translation_sha256','translation_changed')}
            changed.append(item)
        require(header_documents==10997 and len(header_fixes)==1 and header_fixes[0]['source_offset']==REPAIR_SOURCE,'Unexpected B11 header fix inventory')
        if not changed and capacity==g['capacity']:
            if companion_report and h[11]!=kl.H.unpack_from(base)[11]:
                amended=bytearray(base);kl.H.pack_into(amended,0,*h);result=bytes(amended)
            else:result=base
        else:
            h[9]=capacity
            out=bytearray(base[:h[8]*512])
            for i,off in enumerate(sorted(entries)):
                e=entries[off];sector=len(out)//512
                struct.pack_into('<4I',out,h[7]*512+16*i,off,e['end'],sector,len(e['doc']))
                out.extend(kl.padded(e['doc']))
                if off in metas:metas[off].update(doc_sector=sector,doc_bytes=len(e['doc']))
            provenance['HITO_B_memory_export']={'schema':SCHEMA,'base_sha256':g['base_sha256'],
                'content_language':'es','changes':changed,
                'historical_ids_and_ordinals_preserved':True,'personal_collections_opened':False,
                'runtime_card_catalog':'C13 y catálogo persistente que sólo se amplía; las fichas retiradas permanecen disponibles',
                'selection_policy':'La acepción contextual aprobada prevalece sobre las propuestas del diccionario',
                'versioned_export_adapter':{'schema':'kotogba.memory_runtime_export/3','adapter_sha256':file_sha(Path(__file__)),'header_fixes':header_fixes}}
            p=json.dumps(provenance,ensure_ascii=False,separators=(',',':')).encode()
            h[13]=len(out)//512;h[14]=len(p);out.extend(kl.padded(p));h[2]=len(out);kl.H.pack_into(out,0,*h)
            result=bytes(out)
        check_h,check_entries=kl.package(result)
        require(len(check_entries)==len(entries),'Pérdida de documentos en exportación')
        for off,e in check_entries.items():require(e==entries[off],'Relectura binaria distinta')
        if companion_report:
            require(check_h[10]==companion_report['ks5_binding'] and check_h[11]==companion_report['output_checksum'],
                    'KL29 no declara el compañero KC15 exportado')
        output.parent.mkdir(parents=True,exist_ok=True)
        staging=output.with_name(output.name+'.writing');staging.write_bytes(result)
        require(file_sha(staging)==digest(result),'Escritura de paquete incompleta')
        if companion is not None:
            kc_staging=output_kc15.with_name(output_kc15.name+'.writing');kc_staging.write_bytes(companion)
            require(file_sha(kc_staging)==companion_report['output_sha256'],'Escritura KC15 incompleta')
            if fault_hook:fault_hook('pair_staged')
            os.replace(kc_staging,output_kc15)
            if fault_hook:fault_hook('pair_kc15_published')
        os.replace(staging,output)
        if fault_hook:fault_hook('pair_kl29_published')
        report={'schema':'kotogba.memory_runtime_export/3','database_id':meta(conn,'database_id'),
            'game_id':game,'base_sha256':g['base_sha256'],'output_sha256':digest(result),'output_bytes':len(result),
            'documents':len(entries),'active_spans':active_count,'changed_documents':changed,'capacity':capacity,
            'max_document_bytes':max(len(e['doc']) for e in entries.values()),
            'historical_cards_and_ordinals_retained':True,
            'source_text_readings_translations_notes_retained':not any(x.get('translation_review',{}).get('translation_changed') for x in changed),
            'source_text_readings_notes_retained':True,
            'reviewed_translation_documents':sum(bool(x.get('translation_review',{}).get('translation_changed')) for x in changed),
            'byte_identical_to_input':result==base,'personal_collections_opened':False}
        report['adapter_sha256']=file_sha(Path(__file__))
        report['versioned_export_contract']=contract
        report['header_fix_details']=header_fixes
        report['compiled_equivalent_header_check']={'passed':True,'documents':header_documents,'strict_dispatch_flag_check':True,'consumer_reference':CONSUMER_HEADER_REFERENCE}
        report['companion_export_required']=bool(report['reviewed_translation_documents'])
        report['installable_pair_validated']=companion is not None
        if companion_report:
            report['kc15_companion']=companion_report
            report['pair_commit_file']=output.name+'.pair.json'
            pair=dict(schema='kotogba.memory_runtime_pair/1',passed=True,
                scope='Coherencia binaria de exportación; no sustituye a las pruebas del ejecutable.',
                database_id=meta(conn,'database_id'),game_id=game,
                base_KL29_sha256=g['base_sha256'],base_KC15_sha256=companion_report['base_sha256'],
                kl29={'path':output.name,'bytes':len(result),'sha256':digest(result),'kc15_checksum':check_h[11]},
                kc15={'path':output_kc15.name,'bytes':len(companion),'sha256':companion_report['output_sha256'],
                      'checksum':companion_report['output_checksum']},
                paired_binding_verified=True,
                recovery='Si falta esta acta o algún hash no coincide, repetir la exportación completa. Nunca instalar una pareja parcial.')
            write_json(output_kc15.with_suffix(output_kc15.suffix+'.json'),companion_report)
        write_json(output.with_suffix(output.suffix+'.json'),report)
        if companion_report:
            # Commit marker comes last. A process cut between file renames is
            # detectable by its absence or by the old manifest's exact hashes.
            write_json(output.with_name(report['pair_commit_file']),pair)
        return report
    finally:
        conn.close()


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--db',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--base-kc15',type=Path,required=True);parser.add_argument('--output-kc15',type=Path,required=True);args=parser.parse_args()
    result=export_runtime(args.db,args.output,base_kc15=args.base_kc15,output_kc15=args.output_kc15)
    print(json.dumps({key:value for key,value in result.items()if key!='changed_documents'},ensure_ascii=False,indent=2))
