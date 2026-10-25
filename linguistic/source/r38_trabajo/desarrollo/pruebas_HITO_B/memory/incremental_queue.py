#!/usr/bin/env python3
"""Cola incremental de sólo lectura; reutiliza dictámenes exactos del historial.

No cambia estados ni acepciones. Un dictamen sobre varias fichas sólo se reutiliza
si documento, rango y expected_after del paquete importado siguen coincidiendo.
El estado reviewed de sus fichas aisladas nunca aprueba una combinación nueva.
"""
from collections import Counter, defaultdict
from contextlib import ExitStack
from pathlib import Path
import argparse
import fcntl
import json
import shutil
import memory as m

SCHEMA = 'kotogba.incremental_review_queue/1'


def jsonlines(path):
    with Path(path).open(encoding='utf-8') as f:
        for line in f:
            if line.strip():
                yield json.loads(line)


class ExactReviews:
    """Sólo las versiones vigentes de dictámenes en paquetes ya importados."""
    def __init__(self, c, game):
        self.by_range = defaultdict(list)
        self.by_document = defaultdict(list)
        if not c.execute("SELECT 1 FROM sqlite_master WHERE name='koto_span_findings'").fetchone():
            return
        packets = {}; translations = {}
        for row in c.execute('SELECT f.*,d.source_offset,d.source_sha256 FROM koto_span_findings f '
                             'JOIN koto_documents d USING(document_id) WHERE f.game_id=? '
                             'ORDER BY f.finding_id', (game,)):
            pid = row['packet_id']
            if pid not in packets:
                p = c.execute('SELECT p.packet_json,i.packet_sha256 FROM koto_packets p '
                              'JOIN koto_imports i USING(packet_id) WHERE p.packet_id=?', (pid,)).fetchone()
                m.require(p is not None, 'Dictamen sin paquete importado')
                payload = json.loads(p['packet_json'])
                m.require(m.digest(payload) == p['packet_sha256'] and
                          payload['schema'] == 'kotogba.span_findings/1' and payload['game_id'] == game,
                          'Paquete de dictámenes alterado o incompatible')
                packets[pid] = ({x['finding_id']: x for x in payload['items']}, p['packet_sha256'],
                                payload['inventory_code'])
            items, ph, inventory = packets[pid]
            m.require(row['finding_id'] in items, 'El paquete no contiene el dictamen vigente')
            it = items[row['finding_id']]
            did=row['document_id']
            if did not in translations:
                d=c.execute('SELECT * FROM koto_documents WHERE document_id=?',(did,)).fetchone()
                translations[did]=m.effective_document(c,d)
            context=translations[did]
            translation_revision=it['evidence'].get('translation_revision',0)
            translation_sha256=it['evidence'].get('translation_sha256')
            m.require(type(translation_revision) is int and translation_revision>=0,
                      'Revisión de traducción del dictamen inválida')
            translation_current=(translation_revision==context['translation_revision'] and
                (translation_sha256==context['translation_sha256'] or
                 translation_sha256 is None and translation_revision==0))
            m.require(it['source_offset'] == row['source_offset'] and
                      it['source_sha256'] == row['source_sha256'] and
                      it['start'] == row['display_start'] and it['end'] == row['display_end'] and
                      it['expected_revision'] + 1 == row['revision'] and
                      it['decision'] == row['decision'] and it['reason'] == row['reason'] and
                      it['reviewer'] == row['reviewer'] and
                      it['evidence'] == json.loads(row['evidence_json']),
                      'El dictamen y su paquete importado no coinciden')
            record = {'finding_id': row['finding_id'], 'packet_id': pid, 'packet_sha256': ph,
                      'inventory_code': inventory, 'finding_revision': row['revision'],
                      'decision': it['decision'], 'reason': it['reason'], 'reviewer': it['reviewer'],
                      'evidence': it['evidence'], 'expected_after': it['expected_after'],
                      'expected_after_sha256': m.digest(it['expected_after']),
                      'translation_revision':translation_revision,'translation_sha256':translation_sha256,
                      'translation_context_current':translation_current}
            self.by_range[(row['document_id'], it['start'], it['end'])].append(record)
            self.by_document[row['document_id']].append((it['start'],it['end'],record))

    def match(self, document_id, start, end, current):
        all_refs = self.by_range.get((document_id, start, end), [])
        exact = [r for r in all_refs if r['expected_after'] == current and r['translation_context_current']]
        stale = [r for r in all_refs if r['expected_after'] != current or not r['translation_context_current']]
        terminal = [r for r in exact if r['decision'] in ('correct', 'corrected')]
        unresolved = [r for r in exact if r['decision'] == 'unresolved']
        conflict = bool(terminal and unresolved)
        return {'reused': bool(terminal) and not conflict,
                'references': terminal if not conflict else [],
                'unresolved_references': unresolved,
                'stale_references': stale,
                'conflict': conflict}


def build(db, analysis, output, game='bprj1', review_packet_size=100, review_offset=0):
    db, analysis, output = map(Path, (db, analysis, output))
    m.require(not output.exists(), 'Usar un directorio nuevo para una cola reproducible')
    m.require(type(review_packet_size) is int and review_packet_size >= 0 and
              type(review_offset) is int and review_offset >= 0, 'Tamaño u offset de paquete inválido')
    metadata = json.loads((analysis/'analysis.json').read_text())
    m.require(metadata['schema'] == 'kotogba.analysis/1', 'Se requiere una exportación de analyze.py')
    inputs = {'analysis.json': m.file_sha(analysis/'analysis.json')}
    for name in ('occurrence_analysis.jsonl', 'dictionary_candidates_es.jsonl', 'segmentation_candidates.jsonl'):
        inputs[name] = m.file_sha(analysis/name)
        m.require(inputs[name] == metadata['files'][name], 'Análisis alterado: '+name)
    proposal_ids = {r['candidate_set_id'] for r in jsonlines(analysis/'dictionary_candidates_es.jsonl')}
    occurrence_analysis = {}
    for row in jsonlines(analysis/'occurrence_analysis.jsonl'):
        oid = row['occurrence_id']
        m.require(oid not in occurrence_analysis, 'Análisis de aparición duplicado')
        m.require(row['candidate_set_id'] in proposal_ids, 'Referencia a propuestas ausente')
        occurrence_analysis[oid] = {
            'card_sha256': m.digest(row['selected_runtime_card']), 'candidate_set_id': row['candidate_set_id'],
            'translation_revision':row.get('translation_revision',0),
            'translation_sha256':row.get('translation_sha256'),
            'ambiguities': row['ambiguities'], 'context_scores': row['context_scores']}
    candidates = defaultdict(list)
    seen = set()
    for item in jsonlines(analysis/'segmentation_candidates.jsonl'):
        cid = 'candidate:'+m.digest({k:item[k] for k in ('document_id','start','end','visible','reason')})
        m.require(cid not in seen, 'Candidato de intervalo duplicado')
        seen.add(cid)
        candidates[item['document_id']].append((cid, item))
    with db.with_name(db.name+'.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH)
        before = m.file_sha(db)
        c = m.connect(db, True)
        try:
            m.check_schema(c)
            c.execute('BEGIN')
            g = c.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
            m.require(g and g['base_sha256'] == metadata['base_sha256'], 'Análisis de otra base o juego')
            reviews = ExactReviews(c, game)
            reasons = defaultdict(list)
            for r in c.execute('SELECT r.* FROM koto_review_reasons r JOIN koto_documents d USING(document_id) '
                               'WHERE d.game_id=? ORDER BY reason_id', (game,)):
                reasons[r['occurrence_id']].append({'reason_id':r['reason_id'],'code':r['code'],
                                                  'details':json.loads(r['details_json'])})
            output.mkdir(parents=True)
            shutil.copyfile(analysis/'dictionary_candidates_es.jsonl', output/'dictionary_candidates_es.jsonl')
            filenames = ('documents.jsonl','pending_occurrences.jsonl','rejected_occurrences.jsonl',
                         'pending_multispan.jsonl','reused_multispan.jsonl','known_span_reviews.jsonl')
            status_counts = Counter()
            counts = Counter()
            reason_counts = Counter()
            pending_ids, rejected_ids = [], []
            multispan_pending_ids = set()
            occurrence_order = {}
            candidate_documents_seen = set()
            with ExitStack() as stack:
                streams = {n:stack.enter_context((output/n).open('w',encoding='utf-8')) for n in filenames}
                def emit(name, item):
                    streams[name].write(m.canonical(item)+'\n')
                for d in c.execute('SELECT * FROM koto_documents WHERE game_id=? ORDER BY source_offset', (game,)):
                    d=m.effective_document(c,d)
                    did = d['document_id']
                    rows = c.execute('SELECT o.*,v.card_json,v.payload_sha256 FROM koto_occurrences o '
                                     'JOIN koto_card_variants v USING(variant_id) WHERE document_id=? '
                                     'AND active=1 ORDER BY display_start,display_end', (did,)).fetchall()
                    spans = m._active_spans(c,did)
                    m.require(len(rows) == len(spans), 'Apariciones y spans divergentes')
                    active = {r['occurrence_id']:r for r in rows}
                    doc_written = False
                    def document():
                        nonlocal doc_written
                        if not doc_written:
                            emit('documents.jsonl', {'document_id':did,'source_offset':d['source_offset'],
                                'source_sha256':d['source_sha256'],'raw_source_sha256':m.digest(d['raw_source']),
                                'raw_hex':d['raw_source'].hex(),'japanese':d['japanese'],
                                'original_kana':d['original_kana'],'spanish':d['spanish'],
                                'translation_revision':d['translation_revision'],'translation_sha256':d['translation_sha256'],
                                'units':[dict(u) for u in c.execute('SELECT * FROM koto_units WHERE document_id=? '
                                                                  'ORDER BY ordinal', (did,))]})
                            counts['context_documents'] += 1
                            doc_written = True
                    for row, span in zip(rows,spans):
                        status_counts[row['status']] += 1
                        occurrence_order[row['occurrence_id']] = (d['source_offset'],row['display_start'],row['display_end'])
                        m.require(m.occurrence_id(did,span['display_byte_start'],span['display_byte_end']) ==
                                  row['occurrence_id'], 'Orden de apariciones divergente')
                        if row['status'] == 'reviewed':
                            continue
                        m.require(row['status'] in ('generated','pending','rejected'), 'Estado desconocido')
                        document()
                        analysis_item = occurrence_analysis.get(row['occurrence_id'])
                        analysis_current = bool(analysis_item and analysis_item['card_sha256'] == row['payload_sha256'] and
                            analysis_item['translation_revision']==d['translation_revision'] and
                            analysis_item['translation_sha256'] in (None,d['translation_sha256']))
                        record = {'occurrence_id':row['occurrence_id'],'document_id':did,
                            'source_offset':d['source_offset'],'source_sha256':d['source_sha256'],
                            'expected_revision':row['revision'],'current_status':row['status'],
                            'expected_card_sha256':row['payload_sha256'],'current_span':span,
                            'reasons':reasons[row['occurrence_id']],
                            'provenance':json.loads(row['provenance_json']),
                            'context_ref':{'file':'documents.jsonl','document_id':did},
                            'dictionary_proposals_ref':{'file':'dictionary_candidates_es.jsonl',
                                'candidate_set_id':analysis_item['candidate_set_id']} if analysis_current else None,
                            'ambiguities':analysis_item['ambiguities'] if analysis_current else [],
                            'context_scores':analysis_item['context_scores'] if analysis_current else [],
                            'analysis_preimage_current':analysis_current,
                            'queue_reason':'rejected_separate' if row['status']=='rejected' else
                                ('analysis_needs_refresh' if not analysis_current else 'no_individual_review_record')}
                        if row['status'] == 'rejected':
                            emit('rejected_occurrences.jsonl',record);rejected_ids.append(row['occurrence_id'])
                        else:
                            emit('pending_occurrences.jsonl',record);pending_ids.append(row['occurrence_id'])
                    for lo,hi,ref in reviews.by_document.get(did,[]):
                        current = [s for s in spans if s['display_byte_start']<hi and lo<s['display_byte_end']]
                        document()
                        emit('known_span_reviews.jsonl',dict(ref,document_id=did,start=lo,end=hi,
                             exact_current_match=ref['expected_after']==current and ref['translation_context_current']))
                        counts['known_span_review_records'] += 1
                    for cid, item in candidates.get(did,[]):
                        candidate_documents_seen.add(did)
                        lo,hi = item['start'],item['end']
                        jp = d['japanese'].encode()
                        m.require(type(lo) is int and type(hi) is int and 0<=lo<hi<=len(jp) and
                                  jp[lo:hi].decode() == item['visible'], 'Candidato fuera de su contexto')
                        m.require(item['source_offset']==d['source_offset'] and
                                  item['japanese']==d['japanese'] and item['spanish']==d['spanish'] and
                                  item.get('translation_revision',0)==d['translation_revision'] and
                                  item.get('translation_sha256',d['translation_sha256'])==d['translation_sha256'],
                                  'El contexto del candidato ha cambiado; regenerar el análisis')
                        current = [s for s in spans if s['display_byte_start']<hi and lo<s['display_byte_end']]
                        m.require(current, 'Candidato sin apariciones actuales')
                        ids = [m.occurrence_id(did,s['display_byte_start'],s['display_byte_end']) for s in current]
                        matched = reviews.match(did,lo,hi,current)
                        if matched['reused']:
                            state, reason = 'reused_review','exact_imported_finding_matches_current_spans'
                        elif matched['conflict']:
                            state, reason = 'pending','conflicting_exact_findings'
                        elif matched['stale_references']:
                            state, reason = 'pending','stored_review_preimage_changed'
                        elif matched['unresolved_references']:
                            state, reason = 'pending','explicitly_unresolved_finding'
                        else:
                            state, reason = 'pending','no_exact_imported_finding'
                        document()
                        record = {'candidate_id':cid,'document_id':did,'source_offset':d['source_offset'],
                            'source_sha256':d['source_sha256'],'start':lo,'end':hi,'visible':item['visible'],
                            'reason':item['reason'],'queue_reason':reason,'queue_status':state,
                            'context_ref':{'file':'documents.jsonl','document_id':did},
                            'current_spans':current,'current_spans_sha256':m.digest(current),
                            'occurrence_ids':ids,'occurrence_statuses':[active[oid]['status'] for oid in ids],
                            'dictionary_proposals':item['dictionary_candidates'],
                            'analysis_cards_current':item['current_cards']==[s['card'] for s in current],
                            'reused_reviews':matched['references'],'stale_reviews':matched['stale_references'],
                            'unresolved_reviews':matched['unresolved_references'],
                            'automatic_semantic_approval':False}
                        emit('reused_multispan.jsonl' if matched['reused'] else 'pending_multispan.jsonl',record)
                        counts[state+'_multispan'] += 1
                        reason_counts[reason] += 1
                        if not matched['reused']:
                            multispan_pending_ids.update(ids)
            m.require(candidate_documents_seen == set(candidates), 'El análisis contiene documentos ajenos')
            ids_files = {'pending_occurrence_ids.json':pending_ids,'rejected_occurrence_ids.json':rejected_ids,
                         'pending_multispan_occurrence_ids.json':sorted(multispan_pending_ids,key=occurrence_order.__getitem__)}
            for name,ids in ids_files.items():
                m.write_json(output/name,ids)
            packet = None
            if review_packet_size and pending_ids[review_offset:review_offset+review_packet_size]:
                result = m.export_review(db,output/'review_next.json',game=game,ids=pending_ids,
                                         limit=review_packet_size,offset=review_offset)
                packet = {'path':'review_next.json','packet_id':result['packet_id'],'items':result['items'],
                          'offset':review_offset,'selection':'generated_or_pending_active_only'}
            m.require(m.file_sha(db)==before, 'La memoria cambió durante la preparación de la cola')
            summary = {'schema':SCHEMA,'base_sha256':g['base_sha256'],'database_id':m.meta(c,'database_id'),
                'memory_revision':g['revision'],'runtime_revision':g['runtime_revision'],
                'database_sha256_before':before,'database_sha256_after':before,'readonly_verified':True,
                'analysis_memory_revision':metadata['memory_revision'],'input_sha256':inputs,
                'active_status_counts':dict(status_counts),'pending_occurrences':len(pending_ids),
                'rejected_occurrences_separate':len(rejected_ids),
                'reviewed_occurrences_excluded_by_default':status_counts['reviewed'],
                'multispan_pending_occurrence_ids':len(multispan_pending_ids),
                'counts':dict(counts),'candidate_reasons':dict(reason_counts),'next_review_packet':packet,
                'scope':'Cola incremental por estados y dictámenes exactos. No aprueba semántica automáticamente, '
                        'no altera la memoria ni los datos de consola y no generaliza un dictamen a un rango mayor.',
                'files':{p.name:m.file_sha(p) for p in sorted(output.iterdir()) if p.is_file()}}
            m.write_json(output/'queue.json',summary)
            return summary
        finally:
            c.close()


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--db',type=Path,required=True);p.add_argument('--analysis',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--game',default='bprj1')
    p.add_argument('--review-packet-size',type=int,default=100);p.add_argument('--review-offset',type=int,default=0)
    a=p.parse_args()
    result=build(a.db,a.analysis,a.output,a.game,a.review_packet_size,a.review_offset)
    print(json.dumps({k:v for k,v in result.items() if k not in ('files','input_sha256')},ensure_ascii=False,indent=2))
