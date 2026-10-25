#!/usr/bin/env python3
"""Proyección B12 de estado finito; derivada de B11 sin cambiar sus guardas.

El codec y la reparación B11 conservan su implementación histórica. Cinco
preimágenes B12 adicionales tienen un adaptador de cabecera finito y revisado.
Este módulo sólo admite el contrato B12 sellado y publica una pareja validada.
"""
from pathlib import Path
import argparse,json,os,struct,sys
HERE=Path(__file__).resolve().parent;MEM=HERE.parent
sys.path.insert(0,str(MEM));sys.path.insert(0,str(HERE))
import memory as m
from continuation_b11 import export_runtime_v3 as b11
import finite_contract_v4 as finite
import kc15_companion_v2 as kc15_companion
import header_dispatch_b12 as header_dispatch
require=m.require;connect=m.connect;check_schema=m.check_schema;kl=m.kl;digest=m.digest
SCHEMA=m.SCHEMA;file_sha=m.file_sha;meta=m.meta;write_json=m.write_json
_runtime_document=header_dispatch.runtime_document
CONSUMER_HEADER_REFERENCE=header_dispatch.CONSUMER_HEADER_REFERENCE
FINITE_CONTRACT_SHA256='beb882829aa60f2dfcc26fa4a070cd11ae571189a2eee7748c6117f50857fe3e'
CONTRACT_FILE=HERE/'FINITE_EXPORT_CONTRACT_v2.json'
DERIVATION_B11_SHA256='e726339b00852591277fa2c44607dc0d9c76b93714a606f29cadad9394d8c9b9'
CONTRACT_HELPER_SHA256='0b2faa8f21dd1bf6f587f9c137177dd6748c12300bdbd46a3d3c67985f15c787'

def _contract(conn,g):
    require(file_sha(MEM/'continuation_b11/export_runtime_v3.py')==DERIVATION_B11_SHA256,'Exportador B11 histórico alterado')
    require(file_sha(HERE/'finite_contract_v4.py')==CONTRACT_HELPER_SHA256,'Verificador del contrato B12 alterado')
    return finite.verify(conn,g,CONTRACT_FILE,FINITE_CONTRACT_SHA256)


def validate_output_paths(db_path, output, base_kc15=None, output_kc15=None):
    """Comprueba también staging y actas: ninguna ruta puede aliasar otra."""
    output=Path(output)
    sidecars=[output.with_suffix(output.suffix+'.json')]
    destinations=[output,output.with_name(output.name+'.writing')]
    sources=[Path(db_path)]
    if base_kc15 is not None:
        companion=Path(output_kc15);sources.append(Path(base_kc15))
        destinations.extend([companion,companion.with_name(companion.name+'.writing')])
        sidecars.extend([companion.with_suffix(companion.suffix+'.json'),output.with_name(output.name+'.pair.json')])
    destinations.extend(sidecars)
    destinations.extend(path.with_name(path.name+'.writing') for path in sidecars)
    paths=[p.resolve() for p in destinations+sources]
    require(len(paths)==len(set(paths)),'Una salida, staging o acta coincide con otra ruta de la pareja o una fuente')
    protected={Path(__file__).resolve(),CONTRACT_FILE.resolve(),(HERE/'finite_contract_v4.py').resolve()}
    if CONTRACT_FILE.is_file():
        declared=json.loads(CONTRACT_FILE.read_text())
        protected.update(finite.binding_path(item) for item in declared['bound_files'])
    require(not(set(paths[:len(destinations)])&protected),'Una salida sobrescribiría una dependencia o el contrato')

def export_runtime(db_path, output, game='bprj1', capacity=None, base_kc15=None, output_kc15=None, fault_hook=None):
    """Export KL29; pass both KC15 paths to publish a validated SD pair.

    A KL29-only projection with revised Spanish is explicitly incomplete for
    installation: the compiled reader obtains its translation from KC15.
    """
    output=Path(output)
    require(output.resolve()!=Path(db_path).resolve(),'El paquete no puede reemplazar la memoria')
    require((base_kc15 is None)==(output_kc15 is None),'Se requieren ambas rutas KC15 para exportar la pareja')
    validate_output_paths(db_path,output,base_kc15,output_kc15)
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
        require(header_documents==10997 and len(header_fixes)==len(header_dispatch.ALL_REPAIR_SOURCES) and
                {fix['source_offset'] for fix in header_fixes}==header_dispatch.ALL_REPAIR_SOURCES,
                'Unexpected finite B11/B12 header repair inventory')
        require(all(fix.get('source_units_cards_history_verified') is True for fix in header_fixes
                    if fix['source_offset'] in header_dispatch.REPAIR_SOURCES),
                'A new B12 header repair lacks its exact reviewed source/card/history evidence')
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
                'versioned_export_adapter':{'schema':'kotogba.memory_runtime_export/4','adapter_sha256':file_sha(Path(__file__)),'header_fixes':header_fixes}}
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
        report={'schema':'kotogba.memory_runtime_export/4','status':'DEVELOPMENT_NOT_INSTALLABLE','installable':False,
            'database_id':meta(conn,'database_id'),
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
        report['binary_pair_validated']=companion is not None
        if companion_report:
            report['kc15_companion']=companion_report
            report['pair_commit_file']=output.name+'.pair.json'
            pair=dict(schema='kotogba.memory_runtime_pair/2',passed=True,
                status='DEVELOPMENT_NOT_INSTALLABLE',installable=False,binary_pair_validated=True,
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
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--db',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--base-kc15',type=Path,required=True);p.add_argument('--output-kc15',type=Path,required=True)
    a=p.parse_args();r=export_runtime(a.db,a.output,base_kc15=a.base_kc15,output_kc15=a.output_kc15)
    print(json.dumps({k:v for k,v in r.items() if k not in ('changed_documents','versioned_export_contract')},ensure_ascii=False))
