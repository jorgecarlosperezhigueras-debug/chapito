#!/usr/bin/env python3
"""Compara todas las apariciones con la memoria y la morfología DI03 heredada.

El resultado es un inventario trazable de candidatos, no una reasignación
automática de fichas. La acepción contextual instalada mantiene prioridad.
"""
from collections import Counter
from functools import lru_cache
from pathlib import Path
import argparse
import json
import re
import sys

import memory as m


def boundaries(text):
    result={0};at=0
    for char in text:
        at+=len(char.encode('utf-8'));result.add(at)
    return result


def control_intervals(text):
    return [(len(text[:x.start()].encode()),len(text[:x.end()].encode()))
            for x in re.finditer(r'\{[^{}]*\}|\[[^\[\]]*\]',text)]


def analyze(db_path,output,game='bprj1'):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    conn=m.connect(db_path,readonly=True)
    dictionary=m.dictionary_module().Dictionary(db_path)
    counts=Counter();seen_candidates={};compound_seen=set()

    @lru_cache(maxsize=None)
    def lookup(text):
        if any(ch in text for ch in '{}[]') or not text.strip():return []
        found=dictionary.lookup(text,limit=64)
        items=[]
        for c in found['candidates']:
            e=c['entry'];senses=[]
            for s in e['senses']:
                meaning='; '.join(g['text'] for g in s['gloss'].get('spa',[]) if g.get('text'))
                if meaning:senses.append({'source_number':s['source_number'],'meaning_es':meaning,
                    'source_status':s.get('translation_origin'),'review':e['translation_review']})
            items.append({'entry_id':e['id'],'matched_form':c['matched_form'],'inflection':c['inflection'],
                'lemmas':[x['text'] for x in e.get('kanji',[])],
                'readings':[x['text'] for x in e.get('readings',[])],'senses_es':senses,
                'status':'pending','pos_constraint':c['pos_constraint']})
        return items

    try:
        m.check_schema(conn);conn.execute('BEGIN')
        g=conn.execute('SELECT * FROM koto_games WHERE game_id=?',(game,)).fetchone()
        m.require(g is not None,'Juego desconocido')
        with (output/'occurrence_analysis.jsonl').open('w',encoding='utf-8') as analysis, \
             (output/'segmentation_candidates.jsonl').open('w',encoding='utf-8') as compound:
            for d in conn.execute('SELECT * FROM koto_documents WHERE game_id=? ORDER BY source_offset',(game,)):
                d=m.effective_document(conn,d)
                counts['documents']+=1;jp=d['japanese'];raw_jp=jp.encode();valid=boundaries(jp)
                controls=control_intervals(jp)
                rows=conn.execute('SELECT o.*,v.card_json FROM koto_occurrences o JOIN koto_card_variants v USING(variant_id) '
                    'WHERE document_id=? AND active=1 ORDER BY display_start,display_end',(d['document_id'],)).fetchall()
                for row in rows:
                    card=json.loads(row['card_json']);lo,hi=row['display_start'],row['display_end']
                    m.require(lo in valid and hi in valid,'Un enlace corta un carácter UTF-8')
                    m.require(not any(a<lo<b or a<hi<b for a,b in controls),'Un enlace corta un marcador variable/control')
                    surface=raw_jp[lo:hi].decode();counts['occurrences']+=1
                    observed_candidates=lookup(surface) if card['type']!=4 else []
                    lemma_candidates=lookup(m.clean_display(card['surface'])) if card['type']!=4 else []
                    combined={m.digest(c):c for c in observed_candidates+lemma_candidates}
                    candidate_id='candidates:'+m.digest(list(combined.values()))
                    seen_candidates[candidate_id]=list(combined.values())
                    scores=[]
                    terms=m.spanish_terms(d['spanish'])|m.spanish_terms(card['meaning_es'])
                    for c in combined.values():
                        for sense in c['senses_es']:
                            scores.append({'entry_id':c['entry_id'],'sense_number':sense['source_number'],
                                           'overlap':len(terms&m.spanish_terms(sense['meaning_es']))})
                    scores.sort(key=lambda s:(-s['overlap'],s['entry_id'],s['sense_number']))
                    reason=[]
                    if not combined and card['type'] not in (1,2,4):reason.append('domain_term_or_no_dictionary_candidate')
                    if len(combined)>1:reason.append('multiple_dictionary_candidates')
                    if scores and scores[0]['overlap']==0:reason.append('no_spanish_lexical_overlap')
                    if observed_candidates and any(c['inflection'] for c in observed_candidates):counts['inflected_occurrences']+=1
                    if card['type'] in (1,2):counts['grammar_occurrences']+=1
                    elif card['type']==4:counts['dynamic_variable_occurrences']+=1
                    else:counts['lexical_occurrences']+=1
                    rec={'occurrence_id':row['occurrence_id'],'document_id':d['document_id'],
                        'translation_revision':d['translation_revision'],'translation_sha256':d['translation_sha256'],
                        'display_byte_start':lo,'display_byte_end':hi,'surface':surface,
                        'selected_runtime_card':card,'selected_review_status':row['status'],
                        'selection_policy':'Se conserva la acepción contextual adoptada; no se elige la primera acepción del diccionario',
                        'candidate_set_id':candidate_id,'context_scores':scores[:12],'ambiguities':reason,
                        'runtime_reassignment':False}
                    analysis.write(m.canonical(rec)+'\n')
                for start in range(len(rows)):
                    first=rows[start];members=[]
                    for stop in range(start,min(start+4,len(rows))):
                        row=rows[stop];card=json.loads(row['card_json'])
                        if card['type']==4:break
                        if members and row['display_start']!=members[-1]['display_end']:break
                        members.append(row)
                        if len(members)<2:continue
                        text=raw_jp[first['display_start']:row['display_end']].decode()
                        if len(text)>32 or re.search(r'\s|[{}\[\]]',text):break
                        options=lookup(text)
                        # Conjugation is anchored to a dictionary lemma; kana
                        # homophones remain candidates, never semantic proof.
                        if not options:continue
                        key=(d['document_id'],first['display_start'],row['display_end'])
                        if key in compound_seen:continue
                        compound_seen.add(key)
                        cards=[json.loads(x['card_json']) for x in members]
                        record={'document_id':d['document_id'],'source_offset':d['source_offset'],
                            'start':first['display_start'],'end':row['display_end'],'visible':text,
                            'japanese':jp,'spanish':d['spanish'],
                            'translation_revision':d['translation_revision'],'translation_sha256':d['translation_sha256'],
                            'occurrence_ids':[x['occurrence_id'] for x in members],
                            'current_cards':cards,'dictionary_candidates':options,
                            'reason':'grammar_inside_dictionary_word_candidate' if any(c['type'] in (1,2) for c in cards)
                                     else 'compound_split_candidate',
                            'status':'pending','demonstrated_error':False,'runtime_reassignment':False}
                        compound.write(m.canonical(record)+'\n');counts[record['reason']]+=1
        with (output/'dictionary_candidates_es.jsonl').open('w',encoding='utf-8') as stream:
            for key,candidates in sorted(seen_candidates.items()):
                stream.write(m.canonical({'candidate_set_id':key,'candidates':candidates})+'\n')
        report={'schema':'kotogba.analysis/1','base_sha256':g['base_sha256'],'memory_revision':g['revision'],
                'counts':dict(counts),'unique_lookups':lookup.cache_info().currsize,
                'reused_morphology_sha256':m.file_sha(m.PROJECT.parent/'dictionary_data/DI03_ES/dictionary.py'),
                'reused_segmentation_authority_sha256':m.file_sha(m.PROJECT/'desarrollo/pruebas_R32/lexicon/build_inline_lexicon.py'),
                'changes_to_runtime':0,'language':'es',
                'scope':'Comparación completa y candidatos de ambigüedad; requiere revisión contextual antes de importar correcciones',
                'files':{p.name:m.file_sha(p) for p in output.glob('*.jsonl')}}
        m.write_json(output/'analysis.json',report);return report
    finally:dictionary.close();conn.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--db',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--game',default='bprj1')
    a=p.parse_args();print(json.dumps(analyze(a.db,a.output,a.game),ensure_ascii=False,indent=2))
