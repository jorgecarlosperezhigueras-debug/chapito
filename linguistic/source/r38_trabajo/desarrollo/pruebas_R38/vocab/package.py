#!/usr/bin/env python3
"""Lossless KL29 reader and narrow inline replacement for R38 validation."""
from pathlib import Path
import hashlib, json, re, struct, unicodedata

H = struct.Struct('<8s14I')
S = struct.Struct('<6H')
C = struct.Struct('<I10H')


def sha(b):
    return hashlib.sha256(b).hexdigest()


def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xffffffff
    return h


def padded(b):
    return b + bytes(-len(b) % 512)


def text(d, at, size):
    if not size:
        assert not at
        return ''
    assert 0 < at <= len(d) - size - 1 and d[at + size] == 0
    return d[at:at + size].decode('utf-8')


def document(d):
    h = H.unpack_from(d)
    assert h[0] == b'KOTOD029' and h[2] == len(d) and h[3] == fnv(d[64:])
    units = []
    display = 0
    for i in range(h[5]):
        a,b,da,dn,ra,rn = S.unpack_from(d,h[4]+12*i)
        units.append(dict(raw_start=a,raw_end=b,display=text(d,da,dn),reading=text(d,ra,rn),
                          display_byte_start=display,display_byte_end=display+dn))
        display += dn
    notes = []
    for i in range(h[7]):
        q = struct.unpack_from('<I8H2I2H',d,h[6]+32*i)
        notes.append((q[:5],text(d,q[5],q[6]),text(d,q[7],q[8]),q[9:]))
    fragments = []
    for i in range(h[11]):
        a,b,at,n = struct.unpack_from('<4I',d,h[10]+16*i)
        fragments.append((a,b,text(d,at,n)))
    wa = (h[14] >> 8) & 65535
    nw,reserved = struct.unpack_from('<2H',d,wa)
    assert not reserved
    words = [struct.unpack_from('<4H',d,wa+4+8*i) for i in range(nw)]
    ia = wa+4+8*nw
    ns=nc=0
    if h[8]!=ia:
        magic,ns,nc,_,_ = struct.unpack_from('<8s4H',d,ia)
        assert magic == b'KOTOV032' and h[8] == ia+16+12*ns+24*nc,(hex(h[1]),magic,ia,h[8])
    cards = []
    for i in range(nc):
        q = C.unpack_from(d,ia+16+12*ns+24*i)
        cards.append(dict(identity=q[0],surface=text(d,q[1],q[2]),reading=text(d,q[3],q[4]),
                          meaning_es=text(d,q[5],q[6]),explanation_es=text(d,q[7],q[8]),type=q[9]))
    spans = []
    for i in range(ns):
        a,b,lo,hi,ci,flags = S.unpack_from(d,ia+16+12*i)
        spans.append(dict(raw_start=a,raw_end=b,display_byte_start=lo,display_byte_end=hi,
                          flags=flags,card=cards[ci]))
    return dict(header=h,raw=d[96:96+h[9]],raw_hash=d[64:96],units=units,notes=notes,
                fragments=fragments,words=words,spans=spans,cards=cards,inline_at=ia,
                spanish=text(d,h[12],h[13]),japanese=''.join(u['display'] for u in units))


def package(b):
    h = H.unpack_from(b)
    assert h[0] == b'KOTOL029' and h[2] == len(b)
    entries = {}
    for i in range(h[3]):
        a,e,s,n = struct.unpack_from('<4I',b,h[7]*512+16*i)
        d = b[s*512:s*512+n]
        assert H.unpack_from(d)[1] == a
        entries[a] = dict(end=e,doc=d)
    assert len(entries) == h[3]
    return h,entries


def wrap(s,ruby=False):
    s=s.replace('«','"').replace('»','"')
    out=[]; width=0
    for token in re.findall(r'\n|[A-Za-zÀ-ÿ0-9]+(?:[’\'-][A-Za-zÀ-ÿ0-9]+)*|.',s):
        if token=='\n':out.append(token);width=0;continue
        n=sum((8 if ruby else 14) if ord(c)>255 else 6 for c in token)
        if width and width+n>224:out.append('\n');width=0
        assert n<=224,(token,n)
        out.append(token);width+=n
    return ''.join(out)


def replace_inline(old,spans):
    old_parsed = document(old)
    h = list(old_parsed['header'])
    ia = old_parsed['inline_at']
    # Keep the original card ordinals and identities available to existing
    # saved lists. Corrected reader spans point to appended complete words;
    # a previously saved historical card still resolves by its identity.
    cards=list(old_parsed['cards']); index={}; encoded_spans=[]
    for i,c in enumerate(cards):
        key=tuple(c[k] for k in ('identity','surface','reading','meaning_es','explanation_es','type'))
        index.setdefault(key,i)
    for s in spans:
        c=s['card'];key=tuple(c[k] for k in ('identity','surface','reading','meaning_es','explanation_es','type'))
        if key not in index:index[key]=len(cards);cards.append(c)
        encoded_spans.append((s['raw_start'],s['raw_end'],s['display_byte_start'],s['display_byte_end'],index[key],s['flags']))
    assert len(cards)<=256 and len(spans)<=512
    strings_at = ia+16+12*len(spans)+24*len(cards)
    delta = strings_at-h[8]
    doc = bytearray(old[:ia]+bytes(strings_at-ia)+old[h[8]:])
    def shift(at,wide=False):
        fmt='<I' if wide else '<H'; v=struct.unpack_from(fmt,doc,at)[0]
        if v:
            assert h[8]<=v<len(old)
            struct.pack_into(fmt,doc,at,v+delta)
    for i in range(h[5]):
        shift(h[4]+i*12+4);shift(h[4]+i*12+8)
    for i in range(h[7]):
        shift(h[6]+i*32+12);shift(h[6]+i*32+16)
    for i in range(h[11]):shift(h[10]+i*16+8,True)
    strings={}; at=strings_at
    while at<len(doc):
        end=doc.index(0,at);s=bytes(doc[at:end]).decode();strings.setdefault(s,(at,end-at));at=end+1
    def add(s):
        if not s:return 0,0
        if s in strings:return strings[s]
        b=s.encode()
        for previous,(off,n) in list(strings.items()):
            if previous.endswith(s):
                result=(off+n-len(b),len(b));strings[s]=result;return result
        result=(len(doc),len(b));doc.extend(b+b'\0');strings[s]=result;return result
    struct.pack_into('<8s4H',doc,ia,b'KOTOV032',len(spans),len(cards),0,0)
    for i,s in enumerate(encoded_spans):S.pack_into(doc,ia+16+12*i,*s)
    for i,c in enumerate(cards):
        fields=[]
        for key in ('surface','reading','meaning_es','explanation_es'):fields.extend(add(c[key]))
        C.pack_into(doc,ia+16+12*len(spans)+24*i,c['identity'],*fields,c['type'],0)
    h[2]=len(doc);h[8]=strings_at;h[12]=h[12]+delta if h[12] else 0;h[3]=fnv(doc[64:])
    H.pack_into(doc,0,*h)
    current=document(bytes(doc))
    for key in ('raw','raw_hash','units','notes','fragments','words','spanish','japanese'):
        assert old_parsed[key]==current[key],(hex(h[1]),key)
    assert current['spans']==spans
    return bytes(doc),delta


def repack(base,entries,changes,manifest):
    h=list(H.unpack_from(base));out=bytearray(base[:h[8]*512])
    provenance=json.loads(base[h[13]*512:h[13]*512+h[14]])
    prov_sources={int(r['id'].split('_')[-1],16):r for r in provenance.get('sources',[])}
    for i,a in enumerate(sorted(entries)):
        row=entries[a];doc=row['doc'];sector=len(out)//512
        struct.pack_into('<4I',out,h[7]*512+i*16,a,row['end'],sector,len(doc))
        out.extend(padded(doc))
        if a in changes and a in prov_sources:
            delta=changes[a]['string_offset_delta'];m=prov_sources[a]
            for key in ('id_offset','kana_offset'):
                if m.get(key):m[key]+=delta
            for note in m.get('notes',[]):
                if note.get('original_body_offset'):note['original_body_offset']+=delta
    provenance['R38_lexical_repair']=manifest
    p=json.dumps(provenance,ensure_ascii=False,separators=(',',':')).encode()
    h[13]=len(out)//512;h[14]=len(p);out.extend(padded(p));h[2]=len(out)
    H.pack_into(out,0,*h)
    return bytes(out)
