#!/usr/bin/env python3
"""Build R39 from explicitly reviewed occurrences, keeping every old card ordinal."""
from pathlib import Path
from collections import defaultdict, Counter
import argparse, copy, json, struct, sys
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2]
sys.path.insert(0,str(ROOT/'desarrollo/pruebas_R38/vocab'))
import package as kl

def load(p):return json.loads(p.read_text())
def dump(p,o):p.write_text(json.dumps(o,ensure_ascii=False,indent=2)+'\n')
def signature(c):return json.dumps(c,ensure_ascii=False,sort_keys=True,separators=(',',':'))

def compact(doc,meta):
    """Repack referenced strings only; all card data and source metadata survive."""
    parsed=kl.document(doc);h=list(parsed['header']);refs=[]
    def ref(at,wide=False):
        fmt='<2I' if wide else '<2H';off,n=struct.unpack_from(fmt,doc,at)
        if off:refs.append((at,wide,kl.text(doc,off,n)))
    for i in range(h[5]):ref(h[4]+12*i+4);ref(h[4]+12*i+8)
    for i in range(h[7]):ref(h[6]+32*i+12);ref(h[6]+32*i+16)
    for i in range(h[11]):ref(h[10]+16*i+8,True)
    ns=len(parsed['spans']);nc=len(parsed['cards']);ia=parsed['inline_at']
    for i in range(nc):
        for field in (4,8,12,16):ref(ia+16+12*ns+24*i+field)
    if h[12]:refs.append((52,True,parsed['spanish']))
    external=[]
    for name,size in [('id_offset','id_bytes'),('kana_offset','kana_bytes')]:
        if meta and meta.get(name):external.append((meta,name,kl.text(doc,meta[name],meta[size])))
    for note in meta.get('notes',[]) if meta else []:
        if note.get('original_body_offset'):external.append((note,'original_body_offset',kl.text(doc,note['original_body_offset'],note['original_body_bytes'])))
    strings=set(s for _,_,s in refs)|set(s for _,_,s in external)
    pool=bytearray();index={}
    for s in sorted(strings,key=lambda s:(-len(s.encode()),s)):
        b=s.encode();found=None
        for other,(off,n) in index.items():
            if other.endswith(s):found=(off+n-len(b),len(b));break
        if found is None:found=(h[8]+len(pool),len(b));pool.extend(b+b'\0')
        index[s]=found
    out=bytearray(doc[:h[8]])+pool
    for at,wide,s in refs:struct.pack_into('<2I' if wide else '<2H',out,at,*index[s])
    for obj,key,s in external:obj[key]=index[s][0]
    h[2]=len(out);h[12]=index[parsed['spanish']][0] if parsed['spanish'] else 0;h[3]=kl.fnv(out[64:]);kl.H.pack_into(out,0,*h)
    after=kl.document(bytes(out))
    for k in ('raw','raw_hash','units','notes','fragments','words','spanish','japanese','cards','spans'):
        assert parsed[k]==after[k],('compaction',hex(h[1]),k)
    return bytes(out)

def build(output):
    prepared=load(HERE/'prepared_requests.json')
    assert all(kl.sha((HERE/name).read_bytes())==digest for name,digest in prepared['input_hashes'].items()),'A review changed after request preparation; regenerate requests.'
    assert prepared['reviewed_groups']==1992 and prepared['reviewed_occurrences']==4928
    assert not any(prepared[k] for k in ('missing_groups','issues','conflicts'))
    baseline=(HERE.parent/'baseline_R38/BPRJ_01.kl29').read_bytes()
    assert prepared['baseline_sha256']==kl.sha(baseline)
    header,entries=kl.package(baseline)
    provenance=json.loads(baseline[header[13]*512:header[13]*512+header[14]])
    metas={int(m['id'].split('_')[-1],16):m for m in provenance['sources']}
    docs={n:kl.document(e['doc']) for n,e in entries.items()}
    reserved={c['identity'] for d in docs.values() for c in d['cards']};ids={};keys={}
    def card(review):
        c={k:kl.wrap(review[{'surface':'lemma'}.get(k,k)],k=='reading') for k in ('surface','reading','meaning_es','explanation_es')}
        c['type']=review['type'];key=signature(c)
        if key not in keys:
            identity=0x4f000000|(kl.fnv(key.encode())&0xffffff)
            while identity in reserved or (identity in ids and ids[identity]!=key):identity=0x4f000000|((identity+1)&0xffffff)
            keys[key]=identity;ids[identity]=key
        c['identity']=keys[key];return c
    edits=defaultdict(list)
    for r in prepared['clusters']:edits[r['source_offset']].append(copy.deepcopy(r))
    for r in load(HERE/'supplementary_span_edits.json'):
        d=docs[r['source_offset']];lo,hi=r['start'],r['end']
        q=dict(r,group_id='supplementary',context_id='finite_grammar_repair',surface=d['japanese'].encode()[lo:hi].decode(),
               covered_reviews=[],review={k:r[k] for k in ('lemma','reading','meaning_es','explanation_es','type')},
               before=[s for s in d['spans'] if s['display_byte_start']<hi and lo<s['display_byte_end']])
        edits[r['source_offset']].append(q)
    output.mkdir(parents=True,exist_ok=True)
    documents={};changes=[];unresolved=[];grammar_changes=[];compactions=[]
    for source,ee in sorted(edits.items()):
        old=entries[source]['doc'];d=docs[source];ee.sort(key=lambda e:(e['start'],e['end']))
        assert all(x['end']<=y['start'] for x,y in zip(ee,ee[1:])),(hex(source),'overlapping approved edits')
        spans=[]
        for s in d['spans']:
            remain=[(s['display_byte_start'],s['display_byte_end'])]
            for e in ee:
                rr=[]
                for lo,hi in remain:
                    if hi<=e['start'] or lo>=e['end']:rr.append((lo,hi));continue
                    if lo<e['start']:rr.append((lo,e['start']))
                    if hi>e['end']:rr.append((e['end'],hi))
                remain=rr
            for lo,hi in remain:
                if (lo,hi)!=(s['display_byte_start'],s['display_byte_end']):
                    unresolved.append(dict(source_id=f'bprj1:BPRJ1_{source:06X}',start=lo,end=hi,surface=d['japanese'].encode()[lo:hi].decode(),before=s))
                else:spans.append(s)
        for e in ee:
            before=e['before'];assert before
            assert not any(s['card']['type']==4 for s in before),(hex(source),'variable absorption')
            c=card(e['review']);replacement=dict(raw_start=min(s['raw_start'] for s in before),raw_end=max(s['raw_end'] for s in before),
                display_byte_start=e['start'],display_byte_end=e['end'],flags=2,card=c)
            spans.append(replacement)
            item={k:v for k,v in e.items() if k!='review'};item.update(e['review']);item['after']=replacement
            changes.append(item)
            gg=[s for s in before if s['card']['type'] in (1,2)]
            if gg:grammar_changes.append(dict(source_id=e['source_id'],start=e['start'],end=e['end'],surface=e['surface'],before=gg,after=replacement))
        spans.sort(key=lambda s:(s['display_byte_start'],s['display_byte_end']))
        new,delta=kl.replace_inline(old,spans)
        meta=metas.get(source)
        if meta:
            for key in ('id_offset','kana_offset'):
                if meta.get(key):meta[key]+=delta
            for note in meta.get('notes',[]):
                if note.get('original_body_offset'):note['original_body_offset']+=delta
        expanded=len(new)
        if len(new)>18432:
            new=compact(new,meta);compactions.append(dict(source=f'{source:06X}',before=expanded,after=len(new)))
        assert len(new)<=18432,(hex(source),len(new),'document capacity')
        documents[f'{source:06X}']=dict(old_sha256=kl.sha(old),new_sha256=kl.sha(new),old_bytes=len(old),new_bytes=len(new),string_offset_delta=delta,compacted=len(new)!=expanded,replacements=len(ee))
        entries[source]['doc']=new
    dump(output/'unresolved_remainders.json',unresolved)
    assert not unresolved,('Unreviewed split remainders',len(unresolved))
    occurrence_ledger=[]
    by_src=defaultdict(list)
    for c in changes:by_src[c['source_offset']].append(c)
    for r in prepared['requests']:
        relevant=[c for c in by_src[r['source_offset']] if c['start']<r['original_end'] and r['original_start']<c['end']]
        assert relevant,(r['group_id'],r['source_id'])
        intervals=[(max(r['original_start'],c['start']),min(r['original_end'],c['end'])) for c in relevant]
        assert sum(hi-lo for lo,hi in intervals)==r['original_end']-r['original_start'],r
        occurrence_ledger.append(dict(r,quality='contextually_reviewed_ES_R39',active_cards=[dict(identity=c['after']['card']['identity'],start=c['start'],end=c['end']) for c in relevant]))
    counts=dict(reviewed_original_occurrences=len(occurrence_ledger),reviewed_groups=prepared['reviewed_groups'],base_sources=header[3],changed_sources=len(edits),
                replacements=len(changes),new_card_identities=len(ids),original_card_identities_retained=len(reserved),
                lexical_grammar_resegmentations=len(grammar_changes),supplementary_grammar_spans=sum(c['group_id']=='supplementary' and c['type'] in (1,2) for c in changes),supplementary_lexical_spans=sum(c['group_id']=='supplementary' and c['type'] in (0,3) for c in changes),
                max_changed_document_bytes=max(x['new_bytes'] for x in documents.values()),compacted_documents=len(compactions),active_pending_automatic_occurrences=0)
    approvals={p.name:kl.sha(p.read_bytes()) for p in [*(HERE/f'decisions_batch_{i}.json' for i in range(3)),HERE/'request_adjustments.json',HERE/'supplementary_span_edits.json']}
    manifest=dict(schema='kotogba.R39.contextual_review.v1',base_sha256=kl.sha(baseline),counts=counts,approvals=approvals,
                  scope='All 4928 active automatic dictionary occurrences left by R38 are explicitly reviewed in their Japanese and Spanish source context.',
                  saved_list_policy='Every original card and ordinal is retained byte-for-byte in its source; corrected active spans use appended identities.',
                  units_policy='Source raw text, kana, visible ruby, translations, teacher notes and fragments are preserved; finite lexical/grammar boundaries are corrected.')
    provenance['R39_contextual_review']=manifest
    h=list(header);out=bytearray(baseline[:h[8]*512])
    for i,source in enumerate(sorted(entries)):
        e=entries[source];sector=len(out)//512
        struct.pack_into('<4I',out,h[7]*512+16*i,source,e['end'],sector,len(e['doc']));out.extend(kl.padded(e['doc']))
        if source in metas:metas[source].update(doc_sector=sector,doc_bytes=len(e['doc']))
    p=json.dumps(provenance,ensure_ascii=False,separators=(',',':')).encode();h[13]=len(out)//512;h[14]=len(p);out.extend(kl.padded(p));h[2]=len(out);kl.H.pack_into(out,0,*h)
    result=bytes(out);(output/'BPRJ_01.kl29').write_bytes(result)
    report=dict(manifest,output_sha256=kl.sha(result),output_bytes=len(result),changes=changes,documents=documents,compactions=compactions,
                grammar_boundary_changes=grammar_changes,whitespace_boundaries_kept=prepared['whitespace_boundaries_kept'])
    dump(output/'repair_report.json',report);dump(output/'occurrence_review_R39.json',occurrence_ledger)
    print(json.dumps(dict(counts,output_sha256=kl.sha(result),output_bytes=len(result)),ensure_ascii=False,indent=2))

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,default=HERE/'candidate');a=ap.parse_args();build(a.output)
