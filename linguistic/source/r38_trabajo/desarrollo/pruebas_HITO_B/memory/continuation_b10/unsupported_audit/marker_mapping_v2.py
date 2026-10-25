"""BPRJ1 marker correspondence v2: complete atoms, never operands by regex.

Structural correspondence does not approve a variable role or prove a runtime
producer. FD/F7 operands retain their literal byte, including FD20..2F. Callers
must separately bind reviewed role metadata and checked capture capabilities.
The frozen v1 importer/module is not edited by this module.
"""
import re

SCHEMA='kotogba.bprj1.marker_mapping/2'
CONTROL_SIZES=(0,1,1,1,3,1,1,0,1,0,0,2,1,1,1,0,2,1,1,1,1,0,0,0,0)
DISPLAY_MARKER=re.compile(r'\{(FD|F7):([0-9A-F]{2})\}')

class MarkerMappingError(ValueError):pass
def require(value,message):
 if not value:raise MarkerMappingError(message)

def source_atoms(raw):
 require(isinstance(raw,(bytes,bytearray,memoryview)),'La fuente debe ser binaria')
 raw=bytes(raw);atoms=[];at=0;ended=False
 while at<len(raw):
  code=raw[at];size=1;command=None
  if code==0xff:
   atoms.append({'raw_start':at,'raw_end':at+1,'code':code,'kind':'end'});at+=1;ended=True;break
  if code==0xfc:
   require(at+1<len(raw),'Control FC truncado');command=raw[at+1]
   require(command<len(CONTROL_SIZES),'Control FC sin tamaño acreditado en BPRJ1')
   size=2+CONTROL_SIZES[command];kind='control'
  elif code in (0xfd,0xf7):size=2;kind='marker'
  elif code in (0xf8,0xf9):size=2;kind='escaped_glyph'
  else:kind='literal'
  require(at+size<=len(raw),'Átomo fuente truncado')
  row={'raw_start':at,'raw_end':at+size,'code':code,'kind':kind}
  if kind=='marker':row.update(token=raw[at+1],visible='{'+('FD' if code==0xfd else 'F7')+':'+f'{raw[at+1]:02X}'+'}')
  if command is not None:row.update(command=command,operands_hex=raw[at+2:at+size].hex())
  atoms.append(row);at+=size
 require(ended,'La fuente no termina con FF')
 require(at==len(raw),'Hay bytes no acreditados después del fin de fuente')
 return atoms

def marker_mapping(document):
 raw=document['raw_source'];japanese=document['japanese']
 require(isinstance(japanese,str),'La transcripción debe ser UTF-8')
 display=list(DISPLAY_MARKER.finditer(japanese))
 # Fixed-width Pokédex labels have NUL padding rather than FF. With no
 # marker prefix in either representation there is no variable to map.
 if not display and b'\xfd' not in raw and b'\xf7' not in raw:return {}
 source=[a for a in source_atoms(raw) if a['kind']=='marker']
 require(len(source)==len(display),'Marcadores binarios y de pantalla ambiguos')
 out={}
 for atom,match in zip(source,display):
  require(atom['visible']==match[0],'Orden o identidad de marcadores divergente')
  lo=len(japanese[:match.start()].encode('utf-8'));hi=lo+len(match[0].encode('utf-8'))
  require((lo,hi) not in out,'Marcador de pantalla duplicado')
  out[(lo,hi)]=(atom['raw_start'],atom['raw_end'],match[0])
 return out
