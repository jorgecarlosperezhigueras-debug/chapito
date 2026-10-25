"""Keep D19's proven bootstrap, relocate its embedded core with two literals.

Keep the original ARM9 bootstrap layout; append the core above its globals.
Update its two DMA literals and include the newly rebuilt ARM7 DS segments.
"""
from pathlib import Path
import struct,json,hashlib,math,runpy
ROOT=Path(__file__).resolve().parents[1]
# Refuse to produce a distributable with a stale corpus or memory binding.
# R17.1's touch-only edit changed KS5 while leaving KC15 linked to the old ID.
packages=ROOT/'_gba/kotogba/packages'
# Static variable layouts must remain within the renderer's bounded scratch.
runpy.run_path(str(ROOT/'desarrollo/pruebas_R21/audit_legacy_variable_capacity.py'))['audit'](packages/'BPRJ_01.ks5')
study=(packages/'BPRJ_01.ks5').read_bytes()
corpus_header=(packages/'BPRJ_01.kc15').read_bytes()[:512]
memory_header=(packages/'BPRJ_01.km7').read_bytes()[:512]
binding=struct.unpack_from('<I',study,12)[0]
assert binding==struct.unpack_from('<I',corpus_header,44)[0]==struct.unpack_from('<I',memory_header,24)[0], 'KS5/KC15/KM7 bindings differ; regenerate before packaging'
def sha(b):return hashlib.sha256(b).hexdigest()
def crc16(data):
 v=65535
 for b in data:
  v^=b
  for _ in range(8):v=(v>>1)^(0xa001 if v&1 else 0)
 return v
def align(n):return (n+511)//512*512
old=(ROOT/'desarrollo/base_D19/kotoGBA_D19_Arranque_Kanjis.nds').read_bytes()
assert sha(old)=='48907f11887a0b62d0406c1339ecc46d6a45b1e2a84d7a0f3a4def79e6cab51c'
start,entry,address,length=struct.unpack_from('<4I',old,0x20)
assert (start,entry,address,length)==(0x4000,0x02000800,0x02000000,0x2c0d0)
core=(ROOT/'desarrollo/fuentes/code/core/arm9/arm9.bin').read_bytes()
assert len(core)%2==0 and len(core)<=256*1024
payload_offset=0x40000;assert payload_offset>length+0x10000
bootstrap=bytearray(old[start:start+length])
assert struct.unpack_from('<I',bootstrap,0x4f64)[0]==0x020089b0
assert struct.unpack_from('<I',bootstrap,0x4f68)[0]==0x80011aa4
struct.pack_into('<I',bootstrap,0x4f64,address+payload_offset)
struct.pack_into('<I',bootstrap,0x4f68,0x80000000|len(core)//2)
arm9=bootstrap+bytes(payload_offset-len(bootstrap))+core
old7,_,ram7,size7=struct.unpack_from('<4I',old,0x30)
elf7=(ROOT/'desarrollo/fuentes/code/core/arm7/GBARunner3.elf').read_bytes()
entry7=struct.unpack_from('<I',elf7,24)[0]
phoff=struct.unpack_from('<I',elf7,28)[0];stride,count=struct.unpack_from('<HH',elf7,42)
segments=[]
for i in range(count):
    kind,offset,virtual,physical,size,memory,flags,alignment=struct.unpack_from('<8I',elf7,phoff+i*stride)
    if kind==1 and size and not flags&0x100000:segments.append((physical,elf7[offset:offset+size]))
assert min(p for p,b in segments)==ram7==entry7==0x02380000
arm7=bytearray(max(p+len(b) for p,b in segments)-ram7)
for p,data in segments:arm7[p-ram7:p-ram7+len(data)]=data
arm7=bytes(arm7)
oldbanner=struct.unpack_from('<I',old,0x68)[0];banner=bytearray(old[oldbanner:oldbanner+0x840])
title='kotoGBA Hito B\nMemoria y resumen\nBPRJ1'.encode('utf-16-le')
for lang in range(6):banner[0x240+lang*256:0x340+lang*256]=title+bytes(256-len(title))
struct.pack_into('<H',banner,2,crc16(banner[0x20:0x840]))
at7=align(start+len(arm9));atbanner=align(at7+len(arm7));used=atbanner+len(banner);total=align(used)
b=bytearray(total);b[:start]=old[:start];b[:12]=b'KOTO HITO B ';b[start:start+len(arm9)]=arm9;b[at7:at7+len(arm7)]=arm7;b[atbanner:used]=banner
struct.pack_into('<I',b,0x2c,len(arm9));struct.pack_into('<4I',b,0x30,at7,entry7,ram7,len(arm7));struct.pack_into('<I',b,0x68,atbanner);struct.pack_into('<I',b,0x80,used);b[0x14]=max(0,(total-1).bit_length()-17)
b[0x12]=0 # DS mode: the launch mode confirmed working by Jorge.
struct.pack_into('<H',b,0x15e,crc16(b[:0x15e]))
assert b[at7:at7+len(arm7)]==arm7
differences=[i for i,(a,c) in enumerate(zip(old[start:start+length],bootstrap)) if a!=c]
assert all(i in range(0x4f64,0x4f6c) for i in differences)
assert b[start+payload_offset:start+payload_offset+len(core)]==core
assert crc16(b[:0x15e])==struct.unpack_from('<H',b,0x15e)[0]
assert crc16(b[atbanner+0x20:atbanner+0x840])==struct.unpack_from('<H',b,atbanner+2)[0]
assert len(b)==total and len(b)%512==0
output=ROOT/'kotoGBA_D23_Disquetes.nds';output.write_bytes(b)
report={'parent':'D19 Arranque Kanjis','nds_bytes':len(b),'nds_sha256':sha(b),'core_bytes':len(core),'core_sha256':sha(core),'core_payload_address':hex(address+payload_offset),'bootstrap_data_preserved':True,'bootstrap_changed_literals':['DMA source','DMA halfword count'],'arm7_rebuilt_from_source':True,'arm7_bytes':len(arm7),'arm7_sha256':sha(arm7),'arm7_elf_sha256':sha(elf7),'header_banner_crc_valid':True,'physical_console_tested':False}
(ROOT/'desarrollo/pruebas_HITO_B/build').mkdir(parents=True,exist_ok=True)
(ROOT/'desarrollo/pruebas_HITO_B/build/package-results.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
