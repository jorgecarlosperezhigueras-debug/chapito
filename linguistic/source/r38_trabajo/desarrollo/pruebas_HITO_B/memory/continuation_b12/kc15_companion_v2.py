#!/usr/bin/env python3
"""KC15 B12: siete traducciones exactas, sin cambiar identidades ni sectores.

Extiende el canal histórico para las cinco plantillas B12 expresamente
revisadas: cuatro documentos con diez fragmentos y una etiqueta con variables.
Conserva las dos traducciones simples ya adoptadas en B11. No modifica v1,
R37, SQLite, unidades originales ni plantillas KL29.

El cuerpo por recuadro conserva {FD:XX} y sus registros <4H>. La cola de
oración completa usa {FD_n}, representación que reconoce LegacyToken; su
decodificación inversa reproduce literalmente el texto aprobado.
"""
from collections import Counter
from pathlib import Path
import json
import re
import struct
import sys

HERE = Path(__file__).resolve().parent
MEM = HERE.parent
if str(MEM) not in sys.path:
    sys.path.insert(0, str(MEM))
import memory as m
import document_translations as translations
import kc15_companion as historical

CONTRACT_FILE = HERE / 'KC15_B12_TRANSLATION_CONTRACT.json'
CONTRACT_SHA256 = 'a0c8458f4b9c0d43aded8a11f334325392d49474ba7506449b420115acd02ce3'
HISTORICAL_HELPER_SHA256 = '45cc5da7ab35808e9d3511fd5da084acc28a7d0599bd4430299e61949b32ad22'
R37_CODEC_SHA256 = '2d10de7430fee70e1059b4409bc57a5d79a64f29b16d040c7a3428bafb30786b'
LAYOUT_SOURCE_SHA256 = '3d0d2cecd14fdd43f3a07fb7591fdfb7c391e61e1194367f0cce05b26a1e0fb1'
NEW_SOURCES = {0x16C2A0, 0x16C686, 0x190A49, 0x3BE976, 0x3D9201}
HISTORICAL_SOURCES = {0x176BA0, 0x1A858A}
ALL_SOURCES = NEW_SOURCES | HISTORICAL_SOURCES
FD = re.compile(r'\{FD:([0-9A-F]{2})\}')
LEGACY_FD = re.compile(r'\{FD_([0-9]{1,3})\}')
H, D, ENTRY = historical.H, historical.D, historical.ENTRY


def load_contract():
    m.require(m.file_sha(CONTRACT_FILE) == CONTRACT_SHA256, 'Contrato finito KC15 alterado')
    m.require(m.file_sha(MEM / 'kc15_companion.py') == HISTORICAL_HELPER_SHA256,
              'El códec histórico KC15 v1 cambió')
    codec = historical.codec()
    m.require(m.file_sha(Path(codec.__file__)) == R37_CODEC_SHA256, 'El códec R37 cambió')
    contract = json.loads(CONTRACT_FILE.read_text())
    m.require(contract['schema'] == 'kotogba.B12_finite_KC15_translation_contract/1' and
              set(contract['new_sources']) == NEW_SOURCES and
              set(contract['historical_sources']) == HISTORICAL_SOURCES,
              'Alcance KC15 distinto de las cinco traducciones y las dos históricas')
    m.require(len(contract['items']) == 7 and {x['source_offset'] for x in contract['items']} == ALL_SOURCES,
              'Inventario KC15 finito incompleto o duplicado')
    for reference in contract['source_evidence']:
        path = (MEM / reference['portable_path']).resolve()
        m.require(path.is_relative_to(MEM.resolve()) and m.file_sha(path) == reference['sha256'],
                  'Prueba portable del consumidor KC15 alterada')
    m.require(m.file_sha(HERE / 'source_evidence/preparar_corpus_r15.py') == LAYOUT_SOURCE_SHA256,
              'Productor original de posiciones de variables alterado')
    packet_binding = contract['new_translation_packet']
    packet_path = (MEM / packet_binding['path']).resolve()
    m.require(packet_path.is_relative_to(MEM.resolve()) and
              m.file_sha(packet_path) == packet_binding['file_sha256'],
              'Archivo de las cinco traducciones distinto del aprobado')
    packet = json.loads(packet_path.read_text())
    m.require(m.digest(packet) == packet_binding['canonical_sha256'] and
              packet['packet_id'] == packet_binding['packet_id'], 'Contenido de traducciones distinto')
    by_source = {item['source_offset']: item for item in packet['items']}
    m.require(set(by_source) == NEW_SOURCES and len(packet['items']) == 5,
              'Selección de traducciones distintas')
    for entry in contract['items']:
        m.require(m.digest(entry['approved_value']) == entry['approved_value_sha256'],
                  'Payload aprobado incoherente')
        if entry['source_offset'] in NEW_SOURCES:
            item = by_source[entry['source_offset']]
            m.require(item['current'] == entry['original_value'] and
                      item['review']['status'] == 'reviewed' and item['review']['decision'] == 'replace' and
                      item['review']['replacement'] == entry['approved_value'],
                      'El contrato no reproduce las decisiones de traducción')
    return contract


def dependency_bindings():
    """Dependencias portables que debe ligar el contrato final de exportación."""
    contract = load_contract()
    paths = ['continuation_b12/kc15_companion_v2.py',
             'continuation_b12/KC15_B12_TRANSLATION_CONTRACT.json',
             'kc15_companion.py', 'document_translations.py',
             contract['new_translation_packet']['path']]
    paths.extend(item['portable_path'] for item in contract['source_evidence'])
    return ([{'root': 'memory', 'path': path} for path in dict.fromkeys(paths)] +
            [{'root': 'project', 'path': 'desarrollo/pruebas_R37/save_supplement.py'}])


def legacy_layout(es):
    """Port literal de layout() R15; offsets en bytes UTF-8, sin cambiar texto."""
    breaks = []; variables = []; width = 0; bytepos = 0
    for match in re.finditer(r'\s+|\S+', es):
        word = match.group(); word_bytes = word.encode()
        if word.isspace():
            if '\n' in word: width = 0
            else: width += 6 * len(word)
            bytepos += len(word_bytes); continue
        parts = list(re.finditer(r'\{(FD|FC|F9|F7|F8):([0-9A-Fa-f:]+)\}|.', word, re.S))
        widths = {'FD': 112, 'F9': 12, 'FC': 0, 'F7': 90, 'F8': 60}
        spanwidth = sum(widths[x.group(1)] if x.group(1) else 14 if ord(x.group()) > 255 else 6 for x in parts)
        if width and width + spanwidth > 224 and bytepos and es.encode()[bytepos - 1] == 32:
            breaks.append(bytepos - 1); width = 0
        for part in parts:
            val = part.group()
            if part.group(1):
                kind = part.group(1); tid = int(part.group(2).split(':')[0], 16)
                variables.append((bytepos, tid + {'FD': 0, 'F9': 256, 'FC': 512, 'F7': 768, 'F8': 1024}[kind], len(val.encode()), width))
                width += widths[kind]
            else: width += 14 if ord(val) > 255 else 6
            bytepos += len(val.encode())
    return breaks, variables


def require_fd_template(text):
    residual = FD.sub('', text)
    m.require('{' not in residual and '}' not in residual, 'Marcador ajeno al alcance FD acreditado')
    return Counter(match.group() for match in FD.finditer(text))


def encode_whole_spanish(text):
    require_fd_template(text)
    encoded = FD.sub(lambda match: '{FD_' + str(int(match[1], 16)) + '}', text)
    m.require(decode_whole_spanish(encoded) == text, 'Codificación de oración completa no reversible')
    return encoded


def decode_whole_spanish(text):
    def replace(match):
        value = int(match[1])
        m.require(0 <= value < 256, 'Alias FD fuera del consumidor nativo')
        return '{FD:' + f'{value:02X}' + '}'
    decoded = LEGACY_FD.sub(replace, text)
    require_fd_template(decoded)
    m.require(encode_alias_without_recursion(decoded) == text, 'Alias de oración completa no canónico')
    return decoded


def encode_alias_without_recursion(text):
    return FD.sub(lambda match: '{FD_' + str(int(match[1], 16)) + '}', text)


def parse_finite_document(document):
    """Equivalente acotado de CorpusSlice/Extra/SelectFragment y LegacyToken.

    Sólo los siete destinos demostrados: sin cola léxica (4) ni gramatical
    (32). El resto del corpus se conserva byte a byte desde la base fijada.
    """
    codec = historical.codec()
    m.require(len(document) >= 32, 'Documento KC15 truncado')
    q = D.unpack_from(document)
    m.require(q[0] == b'KOTODO15' and q[2] in ALL_SOURCES and len(document) == 32 + q[6] and
              len(document) <= 1536 and codec.fnv(document[32:]) == q[1],
              'Cabecera o checksum KC15 finito inválido')
    raw_size, es_size, flags = q[3], q[4], q[5]
    low = flags & 255; breaks_count = (flags >> 8) & 255; variables_count = flags >> 16
    m.require(low in (0, 1 | 2 | 8, 1 | 2 | 8 | 16), 'Extensión KC15 fuera del alcance finito')
    m.require(0 < raw_size <= 512 and 0 < es_size <= 1024, 'Tamaño de firma o castellano inválido')
    at = 32 + raw_size; end = at + es_size
    m.require(end < len(document) and document[end] == 0 and b'\0' not in document[at:end],
              'Castellano KC15 sin límites exactos')
    signature = document[32:at]; display = document[at:end].decode('utf-8'); at = end + 1
    require_fd_template(display)
    m.require(at + 2 * breaks_count + 8 * variables_count <= len(document), 'Tablas de layout fuera del documento')
    breaks = list(struct.unpack_from('<' + str(breaks_count) + 'H', document, at)) if breaks_count else []
    at += 2 * breaks_count
    variables = [struct.unpack_from('<4H', document, at + 8 * i) for i in range(variables_count)]
    at += 8 * variables_count
    m.require((breaks, variables) == legacy_layout(display), 'Layout KC15 distinto del productor original')
    es_bytes = display.encode()
    m.require(all(0 <= offset < es_size and es_bytes[offset] == 32 for offset in breaks), 'Salto fuera de un espacio')
    for position, token, size, width in variables:
        m.require(0 <= token < 256 and 0 <= position < position + size <= es_size and
                  es_bytes[position:position + size] == ('{FD:' + f'{token:02X}' + '}').encode() and width <= 65535,
                  'Registro de variable no corresponde a su plantilla literal')
    fragments = []; mapping = []; full_raw = None; whole_encoded = None; whole = None
    if flags & 2:
        m.require(at + 2 <= len(document), 'Falta contador de fragmentos')
        count = struct.unpack_from('<H', document, at)[0]; at += 2
        m.require(0 < count <= 32 and at + 6 * count <= len(document), 'Tabla de fragmentos fuera del documento')
        mapping = [struct.unpack_from('<3H', document, at + 6 * i) for i in range(count)]; at += 6 * count
        previous = 0; previous_text = 0
        for index, (start, stop, text_at) in enumerate(mapping):
            m.require(start <= stop <= 1024 and start >= previous and text_at < es_size and
                      (text_at == 0 if index == 0 else text_at > previous_text and es_bytes[text_at - 1] == 10),
                      'Identidad o offset de fragmento incompatible con CorpusSelectFragment')
            text_end = mapping[index + 1][2] - 1 if index + 1 < count else es_size
            m.require(text_at <= text_end <= es_size, 'Límite de texto de fragmento inválido')
            fragments.append([start, stop, es_bytes[text_at:text_end].decode('utf-8')])
            previous = stop + 1; previous_text = text_at
    if flags & 8:
        m.require(flags & 2 and at + 2 <= len(document), 'Fuente completa sin tabla de fragmentos')
        size = struct.unpack_from('<H', document, at)[0]; at += 2
        m.require(0 < size <= 512 and at + size <= len(document) and document[at + size - 1] == 255,
                  'Fuente completa fuera del límite nativo')
        full_raw = document[at:at + size]; at += size
    if flags & 16:
        m.require(flags & 8 and at + 2 <= len(document), 'Oración completa sin fuente acreditada')
        size = struct.unpack_from('<H', document, at)[0]; at += 2
        m.require(0 < size <= 512 and at + size < len(document) and document[at + size] == 0 and
                  b'\0' not in document[at:at + size], 'Oración completa fuera de la capacidad nativa')
        whole_encoded = document[at:at + size].decode('utf-8'); at += size + 1
        whole = decode_whole_spanish(whole_encoded)
    m.require(at == len(document), 'Cola KC15 inesperada o incompleta')
    return {'header': q, 'signature': signature, 'display_spanish': display,
            'breaks': breaks, 'variables': variables, 'fragments': fragments,
            'fragment_mapping': mapping, 'full_raw': full_raw,
            'whole_encoded': whole_encoded, 'whole_spanish': whole}


def verify_source_document(conn, document, entry):
    binding = entry['source_binding']
    units = [dict(row) for row in conn.execute('SELECT * FROM koto_units WHERE document_id=? ORDER BY ordinal',
                                             (document['document_id'],))]
    m.require(document['source_offset'] == entry['source_offset'] and document['document_id'] == entry['document_id'] and
              document['source_end'] == binding['source_end'] and document['source_sha256'] == binding['source_sha256'] and
              m.digest(document['original_bytes']) == binding['source_sha256'] and
              m.digest(document['raw_source']) == binding['raw_source_sha256'] and
              m.digest(document['original_kana'].encode()) == binding['original_kana_sha256'] and
              m.digest(document['japanese'].encode()) == binding['japanese_sha256'] and
              m.digest(units) == binding['source_units_sha256'], 'Fuente, texto japonés o unidades originales cambiados')
    m.require(translations.original(document) == entry['original_value'], 'Traducción original distinta del contrato')
    translations.validate_value(document, entry['approved_value'])


def compile_document(conn, document, row, entry):
    """Serializa una entrada contractual; no adopta una traducción en SQLite."""
    codec = historical.codec(); verify_source_document(conn, document, entry)
    before = parse_finite_document(row['doc']); preimage = entry['kc15_preimage']
    m.require(row['source'] == entry['source_offset'] and row['end'] == document['source_end'] and
              all(row[key] == preimage[key] for key in ('index', 'sector', 'allocation', 'size', 'owner', 'page', 'end')) and
              row['owner'] == row['page'] == 0 and m.digest(row['doc']) == preimage['document_sha256'] and
              row['header'][5] == preimage['flags'] and row['raw'].hex() == preimage['signature_hex'] and
              row['raw'] == codec.normalize(document['raw_source']), 'Preimagen o ruta KC15 distinta')
    old, approved = entry['original_value'], entry['approved_value']
    fragmented = bool(old['fragments'])
    expected_display = '\n'.join(fragment[2] for fragment in old['fragments']) if fragmented else old['spanish']
    m.require(before['display_spanish'] == expected_display and before['fragments'] == old['fragments'],
              'La preimagen KC15 no reproduce los fragmentos originales')
    m.require(before['full_raw'] == (document['raw_source'] if fragmented else None), 'Fuente completa KC15 distinta')
    m.require(before['whole_encoded'] is None, 'La preimagen finita incorporó una oración completa inesperada')
    if fragmented:
        m.require([tuple(fragment[:2]) for fragment in old['fragments']] == codec.intervals(document['raw_source']),
                  'Los fragmentos no son los intervalos originales R37')
    display = '\n'.join(fragment[2] for fragment in approved['fragments']) if fragmented else approved['spanish']
    m.require(require_fd_template(display) == require_fd_template(expected_display), 'Cambio de variables entre recuadros')
    breaks, variables = legacy_layout(display); encoded = display.encode()
    m.require(len(encoded) <= 1024 and len(breaks) <= 255 and len(variables) <= 65535,
              'Texto o layout excede la cabecera nativa')
    flags = (len(breaks) << 8) | (len(variables) << 16); extra = bytearray()
    whole_encoded = None
    if fragmented:
        flags |= 1 | 2 | 8 | 16
        extra += struct.pack('<H', len(approved['fragments'])); text_at = 0
        for start, stop, text in approved['fragments']:
            extra += struct.pack('<3H', start, stop, text_at); text_at += len(text.encode()) + 1
        raw = document['raw_source']; extra += struct.pack('<H', len(raw)) + raw
        whole_encoded = encode_whole_spanish(approved['spanish'])
        whole = whole_encoded.encode(); m.require(0 < len(whole) <= 512, 'Oración completa demasiado grande')
        extra += struct.pack('<H', len(whole)) + whole + b'\0'
    body = (row['raw'] + encoded + b'\0' + b''.join(struct.pack('<H', position) for position in breaks) +
            b''.join(struct.pack('<4H', *variable) for variable in variables) + extra)
    new = D.pack(b'KOTODO15', codec.fnv(body), row['source'], len(row['raw']), len(encoded), flags, len(body)) + body
    m.require(len(new) <= 1536 and len(new) <= row['allocation'],
              'La traducción finita no cabe en los sectores existentes; no se publica una reasignación implícita')
    decoded = parse_finite_document(new)
    m.require(decoded['signature'] == before['signature'] and decoded['display_spanish'] == display and
              decoded['fragments'] == approved['fragments'] and decoded['full_raw'] == before['full_raw'] and
              (decoded['whole_spanish'] if fragmented else decoded['display_spanish']) == approved['spanish'],
              'La relectura nativa no reproduce la traducción y sus fragmentos aprobados')
    return new, {'source_offset': row['source'], 'document_id': entry['document_id'], 'group': entry['group'],
        'index': row['index'], 'sector': row['sector'], 'owner': row['owner'], 'page': row['page'],
        'allocation_bytes': row['allocation'], 'before_bytes': row['size'], 'after_bytes': len(new),
        'before_sha256': m.digest(row['doc']), 'after_sha256': m.digest(new),
        'before_flags': row['header'][5], 'after_flags': flags,
        'fragment_mapping': decoded['fragment_mapping'], 'fragments': approved['fragments'],
        'display_spanish': display, 'whole_spanish_encoded': whole_encoded,
        'whole_spanish_decoded': decoded['whole_spanish'], 'approved_whole_spanish': approved['spanish'],
        'spanish_breaks': breaks, 'variable_records': variables,
        'source_signature_sha256': m.digest(row['raw']), 'full_source_unchanged': True,
        'approved_value_sha256': entry['approved_value_sha256'], 'consumer_roundtrip_exact': True}


def export_bytes(conn, base_path, game='bprj1'):
    """Sólo lectura: requiere las siete traducciones ya adoptadas exactamente."""
    contract = load_contract(); codec = historical.codec(); base_path = Path(base_path)
    m.require(game == contract['game_id'] and m.meta(conn, 'database_id') == contract['database_id'], 'Otra memoria o juego')
    base = base_path.read_bytes()
    m.require(m.digest(base) == contract['base_kc15_sha256'] == historical.BASE_KC15_SHA256[game], 'KC15 base distinto')
    h, rows, hashes = historical.parse(base)
    game_row = conn.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
    m.require(game_row is not None and game_row['base_sha256'] == contract['runtime_base_sha256'], 'Base lingüística distinta')
    kh = m.kl.H.unpack_from(game_row['base_payload'])
    m.require((h[10], h[11]) == (kh[10], kh[11]), 'Compañero KC15 ajeno a la base KL29 original')
    translated = {row[0] for row in conn.execute('SELECT DISTINCT document_id FROM koto_document_translations')}
    m.require(translated == {entry['document_id'] for entry in contract['items']}, 'Inventario de traducciones fuera del contrato finito KC15')
    out = bytearray(base); changes = []; parsed_packets = {}
    raw_counts = Counter(row['raw'] for row in rows.values())
    signature_counts = Counter(row['signature'] for row in rows.values())
    for entry in contract['items']:
        document = conn.execute('SELECT * FROM koto_documents WHERE document_id=?', (entry['document_id'],)).fetchone()
        m.require(document is not None, 'Falta una fuente aprobada')
        state = translations.state(conn, document)
        m.require(state['revision'] == entry['translation_revision'] and state['packet_id'] == entry['translation_packet_id'] and
                  state['value'] == entry['approved_value'] and state['sha256'] == entry['approved_value_sha256'],
                  'La traducción efectiva aún no es la revisión aprobada exacta')
        pid = state['packet_id']
        if pid not in parsed_packets:
            stored = conn.execute('SELECT p.packet_json,i.packet_sha256 FROM koto_packets p JOIN koto_imports i USING(packet_id) WHERE packet_id=?', (pid,)).fetchone()
            m.require(stored is not None, 'Traducción sin adopción registrada')
            parsed_packets[pid] = json.loads(stored['packet_json'])
            m.require(m.digest(parsed_packets[pid]) == stored['packet_sha256'], 'Paquete almacenado alterado')
        m.require(m.digest(parsed_packets[pid]) == entry['translation_packet_sha256'], 'Otro paquete de traducción que el aprobado')
        source = entry['source_offset']; row = rows[source]
        routes = [list(route) for route in hashes if route[0] == row['signature']]
        m.require(routes == entry['kc15_preimage']['routes'], 'Ruta hash del destino KC15 modificada')
        dynamic = bool(FD.search(entry['original_value']['spanish']))
        if dynamic:
            m.require(not routes, 'Una plantilla dinámica no puede introducir una ruta hash sin procedencia')
        else:
            m.require(raw_counts[row['raw']] == signature_counts[row['signature']] == 1 and
                      routes == [[row['signature'], row['index'], len(row['raw']), codec.fnv(document['raw_source'])]],
                      'Firma estática con alias, colisión o ruta de otra fuente')
        new, detail = compile_document(conn, document, row, entry)
        at, stop = row['at'], row['at'] + row['allocation']
        m.require(not any(base[at + row['size']:stop]), 'Relleno original no vacío; no se sobrescribe')
        out[at:stop] = new + bytes(row['allocation'] - len(new))
        changes.append({**detail, 'translation_revision': state['revision'], 'translation_sha256': state['sha256'], 'packet_id': pid})
    struct.pack_into('<I', out, 48, codec.fnv(out[512:]))
    result = bytes(out); final_h, final_rows, final_hashes = historical.parse(result)
    m.require(final_h[:11] == h[:11] and final_h[12:] == h[12:] and final_hashes == hashes and
              result[512:h[9] * 512] == base[512:h[9] * 512], 'Se alteraron identidades o directorios KC15')
    for source, row in rows.items():
        after = final_rows[source]
        m.require(all(row[key] == after[key] for key in ('end', 'sector', 'owner', 'page', 'index', 'raw', 'allocation')),
                  'Se alteró una identidad, firma o asignación de sectores KC15')
        if source not in ALL_SOURCES:
            m.require(row['doc'] == after['doc'], 'Cambió un documento KC15 ajeno a las siete traducciones')
    m.require(len(changes) == 7 and sum(len(item['fragments']) for item in changes) == 10,
              'Falta una traducción o fragmento finito')
    report = {'schema': 'kotogba.memory_kc15_export/2', 'status': 'DEVELOPMENT_NOT_INSTALLABLE', 'installable': False,
        'game_id': game, 'base_sha256': m.digest(base), 'output_sha256': m.digest(result), 'output_bytes': len(result),
        'base_checksum': h[11], 'output_checksum': final_h[11], 'ks5_binding': h[10],
        'documents': len(rows), 'hash_entries': len(hashes), 'reviewed_documents': 7,
        'new_translation_documents': 5, 'historical_translation_documents': 2, 'reviewed_fragments': 10,
        'changed_documents': changes, 'unchanged_documents': len(rows) - 7,
        'indices_owners_pages_signatures_unchanged': True, 'sectors_reassigned': 0,
        'byte_identical_to_input': False, 'personal_collections_opened': False,
        'finite_translation_contract_sha256': CONTRACT_SHA256, 'adapter_sha256': m.file_sha(Path(__file__)),
        'codec_path': str(Path(codec.__file__).resolve().relative_to(m.PROJECT)), 'codec_sha256': R37_CODEC_SHA256,
        'portable_native_references': contract['source_evidence'],
        'supported_scope': 'Cinco traducciones B12 y dos simples históricas B11, con payloads exactos; recuadros y oración completa se comprueban por separado.'}
    return result, report
