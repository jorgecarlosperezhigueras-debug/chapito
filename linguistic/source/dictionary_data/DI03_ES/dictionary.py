"""kotoGBA offline JMdict adapter. Automatic candidates never become reviews."""
from pathlib import Path
import argparse, collections, gzip, hashlib, json, re, sqlite3
from lxml import etree

ROOT = Path(__file__).resolve().parent
XML_LANG = '{http://www.w3.org/XML/1998/namespace}lang'

def import_jmdict(path, destination=ROOT/'jmdict.sqlite'):
    path, destination = Path(path), Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.building.sqlite')
    if temporary.exists(): temporary.unlink()
    db = sqlite3.connect(temporary)
    db.executescript('''PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
      CREATE TABLE entries(id INTEGER PRIMARY KEY, payload TEXT NOT NULL);
      CREATE TABLE forms(form TEXT NOT NULL, entry_id INTEGER NOT NULL, kind TEXT NOT NULL,
                         UNIQUE(form,entry_id,kind));
      CREATE INDEX form_index ON forms(form);
      CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);''')
    stats = collections.Counter()
    with gzip.open(path, 'rb') as f:
        header = f.read(28000).decode('utf8')
    entities = dict(re.findall(r'<!ENTITY\s+([\w-]+)\s+"([^"]+)"', header))
    reverse = {v:k for k,v in entities.items()}
    with gzip.open(path, 'rb') as stream:
        # Trust only the downloaded snapshot's local DTD; no external entities.
        context = etree.iterparse(stream, events=('end',), tag='entry',
                                 resolve_entities=True, no_network=True)
        for _, node in context:
            entry_id = int(node.findtext('ent_seq'))
            kanji = [{'text':k.findtext('keb'), 'info':[reverse.get(x.text,x.text) for x in k.findall('ke_inf')],
                      'priority':[x.text for x in k.findall('ke_pri')]} for k in node.findall('k_ele')]
            readings = [{'text':r.findtext('reb'), 'restrict':[x.text for x in r.findall('re_restr')],
                         'no_kanji':r.find('re_nokanji') is not None,
                         'info':[reverse.get(x.text,x.text) for x in r.findall('re_inf')],
                         'priority':[x.text for x in r.findall('re_pri')]} for r in node.findall('r_ele')]
            senses=[]; inherited_pos=[]
            for number, s in enumerate(node.findall('sense'), 1):
                pos=[reverse.get(x.text,x.text) for x in s.findall('pos')]
                if pos: inherited_pos=pos
                gloss={}
                for g in s.findall('gloss'):
                    lang=g.get(XML_LANG,'eng')
                    if lang in ('eng','spa'):
                        gloss.setdefault(lang,[]).append({'text':g.text or '',
                          'type':g.get('g_type'), 'gender':g.get('g_gend')})
                if not gloss: continue
                senses.append({'source_number':number,'pos':pos or inherited_pos,
                  'restrict_kanji':[x.text for x in s.findall('stagk')],
                  'restrict_reading':[x.text for x in s.findall('stagr')], 'gloss':gloss,
                  'notes':[x.text for x in s.findall('s_inf')],
                  'misc':[reverse.get(x.text,x.text) for x in s.findall('misc')],
                  'field':[reverse.get(x.text,x.text) for x in s.findall('field')],
                  'dialect':[reverse.get(x.text,x.text) for x in s.findall('dial')],
                  'crossrefs':[x.text for x in s.findall('xref')]})
            if senses:
                record={'id':f'jmdict:{entry_id}','kanji':kanji,'readings':readings,'senses':senses}
                db.execute('INSERT INTO entries VALUES(?,?)',(entry_id,json.dumps(record,ensure_ascii=False,separators=(',',':'))))
                for kind, forms in [('kanji',kanji),('reading',readings)]:
                    db.executemany('INSERT OR IGNORE INTO forms VALUES(?,?,?)',[(r['text'],entry_id,kind) for r in forms])
                stats['entries']+=1
                if any('spa' in s['gloss'] for s in senses):stats['entries_with_spanish']+=1
                if any('eng' in s['gloss'] for s in senses):stats['entries_with_english']+=1
            node.clear()
            while node.getprevious() is not None: del node.getparent()[0]
    stats['indexed_forms']=db.execute('SELECT COUNT(*) FROM forms').fetchone()[0]
    stats['unique_forms']=db.execute('SELECT COUNT(DISTINCT form) FROM forms').fetchone()[0]
    manifest={'schema':'kotogba.dictionary/1','checkpoint':'DI01','source':'JMdict / EDRDG',
       'source_url':'http://ftp.edrdg.org/pub/Nihongo/JMdict.gz',
       'snapshot_date':re.search(r'JMdict created: ([\d-]+)',header)[1],
       'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest(), 'counts':dict(stats),
       'language_policy':'Spanish when supplied; original English fallback explicitly labelled. No automatic translation or sense alignment.',
       'candidate_review':'automatic_dictionary_candidates_not_contextually_reviewed',
       'proper_names':'Game-specific names may be absent; unresolved forms stay unresolved.',
       'grammar':'Inflection candidates only; reviewed grammar remains authoritative.',
       'native_status':'See native-pilot.json; full SQLite is the reusable content backend, not an NDS SQLite runtime.',
       'takoboto':'Uses JMdict, the vocabulary source acknowledged by Takoboto. Not a Takoboto API or affiliation.'}
    for key,value in manifest.items():db.execute('INSERT INTO metadata VALUES(?,?)',(key,json.dumps(value,ensure_ascii=False)))
    db.commit();db.close();temporary.replace(destination)
    (destination.parent/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
    return manifest

# Bounded candidate rules authored for kotoGBA; no grammatical meaning is selected.
# Each terminal verb candidate is checked against JMdict's part of speech.
STEMS=[('い','う','v5u'),('き','く','v5k'),('ぎ','ぐ','v5g'),('し','す','v5s'),
       ('ち','つ','v5t'),('に','ぬ','v5n'),('び','ぶ','v5b'),('み','む','v5m'),('り','る','v5r')]
RULES=[]
for polite in ['ます','ました','ません','ませんでした']:
    RULES.append((polite,'る','v1','forma cortés'))
    for stem,last,pos in STEMS:RULES.append((stem+polite,last,pos,'forma cortés'))
    RULES.extend([('し'+polite,'する','vs-i','forma cortés'),('き'+polite,'くる','vk','forma cortés')])
for terminal,label in [('て','forma て'),('た','forma た'),('たら','forma たら')]:
    RULES.append((terminal,'る','v1',label))
    for tail,lasts in [('っ', [('う','v5u'),('つ','v5t'),('る','v5r'),('く','v5k-s')]),
                       ('ん',[('む','v5m'),('ぶ','v5b'),('ぬ','v5n')]),
                       ('い',[('く','v5k'),('ぐ','v5g')]),('し',[('す','v5s')])]:
        for last,pos in lasts:
            term=terminal
            if last in ('む','ぶ','ぬ','ぐ'):term={'て':'で','た':'だ','たら':'だら'}[terminal]
            RULES.append((tail+term,last,pos,label))
    RULES.extend([('し'+terminal,'する','vs-i',label),('き'+terminal,'くる','vk',label)])
for negative,label in [('ない','forma negativa'),('なかった','negativo pasado')]:
    RULES.append((negative,'る','v1',label))
    for stem,last,pos in [('わ','う','v5u'),('か','く','v5k'),('が','ぐ','v5g'),('さ','す','v5s'),
                          ('た','つ','v5t'),('な','ぬ','v5n'),('ば','ぶ','v5b'),('ま','む','v5m'),('ら','る','v5r')]:
        RULES.append((stem+negative,last,pos,label))
    RULES.extend([('し'+negative,'する','vs-i',label),('こ'+negative,'くる','vk',label),('く'+negative,'い','adj-i',label)])
RULES.extend([('かった','い','adj-i','adjetivo pasado')])
UNWRAP=[('ている','て','forma ている'),('でいる','で','forma ている'),
        ('てる','て','contracción de ている'),('でる','で','contracción de ている'),
        ('ていた','て','forma ていた'),('でいた','で','forma ていた'),
        ('てた','て','contracción de ていた'),('でた','で','contracción de ていた')]

def variants(text):
    result=[(text,None,[])];seen={(text,None)}
    # Wrapper transformations may chain, terminal verb transformations may not.
    queue=[(text,[],0)]
    for form,chain,depth in queue:
        if depth>=2:continue
        for suffix,replacement,label in UNWRAP:
            if form.endswith(suffix) and len(form)>len(suffix):
                v=form[:-len(suffix)]+replacement
                if (v,None) not in seen:seen.add((v,None));queue.append((v,chain+[label],depth+1))
    for form,chain,depth in queue:
        for suffix,replacement,pos,label in RULES:
            if form.endswith(suffix) and len(form)>len(suffix):
                v=form[:-len(suffix)]+replacement
                if (v,pos) not in seen:seen.add((v,pos));result.append((v,pos,chain+[label]))
    return result[:256]

class Dictionary:
    def __init__(self, path=ROOT/'jmdict.sqlite'):
        self.db=sqlite3.connect(f'file:{Path(path).resolve()}?mode=ro',uri=True)
        self.cache={}
    def entries(self,form):
        if form not in self.cache:
            self.cache[form]=[json.loads(row[0]) for row in self.db.execute(
              'SELECT e.payload FROM forms f JOIN entries e ON e.id=f.entry_id WHERE f.form=? GROUP BY e.id ORDER BY e.id',(form,))]
        return self.cache[form]
    def lookup(self,text,limit=24):
        result=[];seen=set()
        for form,pos,chain in variants(text):
            for entry in self.entries(form):
                allowed={pos}
                if pos=='v5k':allowed.add('v5k-s')
                if pos=='v5r':allowed.add('v5r-i')
                compatible=any(allowed.intersection(s['pos']) for s in entry['senses']) if pos else True
                if not compatible:continue
                key=(entry['id'],form,tuple(chain))
                if key in seen:continue
                seen.add(key)
                result.append({'entry':spanish_display(entry),'matched_form':form,'observed':text,
                  'inflection':chain,'pos_constraint':pos,'status':'dictionary_candidate',
                  'meaning_selected_in_context':False})
        return {'observed':text,'candidates':result[:limit],'total_candidates':len(result),
                'truncated':len(result)>limit,'review':'not_contextually_reviewed'}
    def chunk(self,text):
        found=self.lookup(text)
        if found['candidates']:return found
        # Candidate lexical boundary, not a judgement on the particle's function.
        for suffix in ['から','まで','より','には','では','は','が','を','に','で','と','も','の','へ','な','だ']:
            if len(text)>len(suffix)+1 and text.endswith(suffix):
                found=self.lookup(text[:-len(suffix)])
                if found['candidates']:
                    found['chunk']=text;found['unanalysed_suffix']=suffix
                    found['boundary_status']='candidate_boundary_not_reviewed'
                    return found
        return found
    def close(self): self.db.close()

JAPANESE_RUN=re.compile(r'[\u3040-\u30ff\u3400-\u9fff\uff66-\uff9dー]+')
TOKENS=re.compile(r'\{[^}]*\}|\[[^\]]*\]')

def spanish_display(entry):
    """Spanish-only user payload; original English stays inside the source DB."""
    result={k:v for k,v in entry.items() if k!='senses'};result['senses']=[]
    methods=set()
    for s in entry['senses']:
        gloss=list(s['gloss'].get('spa',[]))
        for g in s.get('gloss_es_auto',[]):
            if g['text'] not in {x['text'] for x in gloss}:gloss.append({k:v for k,v in g.items() if k!='source_language'})
            methods.add(g['method'])
        if not gloss:continue
        out={k:v for k,v in s.items() if k not in ('gloss','notes','gloss_es_auto','notes_es')}
        out['gloss']={'spa':gloss};out['notes']=[x['text'] for x in s.get('notes_es',[])]
        methods.update(x['method'] for x in s.get('notes_es',[]))
        out['translation_origin']='original_spanish' if s['gloss'].get('spa') else 'translated_from_english'
        result['senses'].append(out)
    result['senses'].sort(key=lambda s:s['translation_origin']!='original_spanish')
    result['display_language']='es-ES'
    result['translation_review']='automatic_pending_review' if any(m.startswith(('opus','nllb','wordnet_','scientific_taxon','jmdict_same_sense')) or 'pending_review' in m for m in methods) else 'source_or_assistant_translation'
    result['pending_translation_count']=sum(g.get('method')=='unresolved_pending_translation' for s in result['senses'] for g in s['gloss']['spa'])
    return result
def chunks(text):
    # Replace controls with equal-length blanks so Unicode coordinates stay exact.
    clean=TOKENS.sub(lambda m:' '*len(m.group()),text)
    for m in JAPANESE_RUN.finditer(clean):yield m.start(),m.end(),m.group()

def source_context(base,source_id,seen_end):
    base=Path(base)
    source=next((json.loads(l) for l in (base/'games/bprj1/sources.jsonl').read_text().splitlines()
                 if json.loads(l)['id']==source_id),None)
    if not source:raise ValueError('source_id desconocido')
    if seen_end is None or seen_end<len(source['study_text']):
        return {'status':'waiting_for_complete_source','source_id':source_id,
                'required_seen_end':len(source['study_text'])}
    annotation=next((json.loads(l) for l in (base/'games/bprj1/annotations.jsonl').read_text().splitlines()
                     if json.loads(l)['source_id']==source_id),None)
    return {'status':'complete_source','source':source,'reviewed_annotation':annotation,
            'translation_alignment':'full_sentence_only_no_word_alignment_inferred'}

def main():
    ap=argparse.ArgumentParser();sub=ap.add_subparsers(dest='command',required=True)
    p=sub.add_parser('import');p.add_argument('snapshot');p.add_argument('--output',default=str(ROOT/'jmdict.sqlite'))
    p=sub.add_parser('lookup');p.add_argument('word');p.add_argument('--database',default=str(ROOT/'jmdict.sqlite'))
    p=sub.add_parser('context');p.add_argument('source_id');p.add_argument('--base',default=str(ROOT.parents[1]));p.add_argument('--seen-end',type=int)
    args=ap.parse_args()
    if args.command=='import':out=import_jmdict(args.snapshot,args.output)
    elif args.command=='lookup':
        d=Dictionary(args.database);out=d.chunk(args.word);d.close()
    else:
        out=source_context(args.base,args.source_id,args.seen_end)
        if out['status']=='complete_source':
            d=Dictionary();out['dictionary']=[{'span':[a,b],**d.chunk(t)} for a,b,t in chunks(out['source']['study_text'])];d.close()
    print(json.dumps(out,ensure_ascii=False,indent=2))
if __name__=='__main__': main()
