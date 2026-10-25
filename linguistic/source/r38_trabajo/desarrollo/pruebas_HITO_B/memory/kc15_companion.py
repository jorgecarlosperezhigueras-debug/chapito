#!/usr/bin/env python3
"""Exportación KC15 acotada desde las revisiones castellanas de la memoria.

Reutiliza el códec R37 para normalizar las firmas y distribuir el castellano.
No vuelve a numerar documentos, índices, fichas ni colecciones. La versión 1
reemplaza documentos simples sin variables, fragmentos ni alias, dentro de
sus sectores existentes. Rechaza cualquier otra revisión antes de publicar.
"""
from collections import Counter
from pathlib import Path
import importlib.util
import struct

import document_translations as translations

H = struct.Struct('<8s14I')
D = struct.Struct('<8s6I')
ENTRY = struct.Struct('<IIIHH')
_CODEC = None
BASE_KC15_SHA256 = {'bprj1': 'df4a3e7cde5eadfd927c4ebc208b9c4c4934c276369d55251df66d9d66f634c3'}


def codec():
    global _CODEC
    if _CODEC is None:
        path = Path(__file__).resolve().parents[3] / 'desarrollo/pruebas_R37/save_supplement.py'
        spec = importlib.util.spec_from_file_location('kotogba_kc15_codec_r37', path)
        _CODEC = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_CODEC)
    return _CODEC


def parse(data):
    """Validación completa de límites, sumas y directorios antes de exportar."""
    m = translations.api(); c = codec()
    m.require(len(data) >= 512 and len(data) % 512 == 0, 'KC15 truncado o no sectorizado')
    h = H.unpack_from(data)
    m.require(h[0] == b'KOTOC015' and h[1] == 1 and h[2] == len(data), 'Cabecera KC15 incompatible')
    m.require(h[11] == c.fnv(data[512:]), 'Suma global KC15 incorrecta')
    m.require(0 < h[7] < h[8] < h[9] and h[9] * 512 <= len(data), 'Directorios KC15 fuera de rango')
    m.require(h[7] * 512 + h[3] * 16 <= h[8] * 512 and
              h[8] * 512 + h[4] * 16 <= h[9] * 512, 'Tablas KC15 solapadas')
    rows = {}; previous = -1; sectors = []
    for index in range(h[3]):
        source, end, sector, owner, page = ENTRY.unpack_from(data, h[7] * 512 + index * 16)
        at = sector * 512
        m.require(source > previous and end > source and h[9] * 512 <= at < len(data), 'Identidad KC15 inválida')
        previous = source
        q = D.unpack_from(data, at)
        size = 32 + q[6]
        m.require(q[0] == b'KOTODO15' and q[2] == source and 32 <= size <= 1536 and
                  at + size <= len(data), 'Documento KC15 incompatible')
        doc = data[at:at + size]
        m.require(q[1] == c.fnv(doc[32:]) and q[3] <= 512 and q[4] <= 1024 and
                  32 + q[3] + q[4] < size, 'Cuerpo KC15 inválido')
        raw = doc[32:32 + q[3]]; es_at = 32 + q[3]
        m.require(doc[es_at + q[4]] == 0, 'Castellano KC15 sin terminador')
        spanish = doc[es_at:es_at + q[4]].decode('utf-8')
        rows[source] = dict(source=source, end=end, sector=sector, owner=owner, page=page,
                            index=index, at=at, size=size, header=q, doc=doc, raw=raw,
                            spanish=spanish, signature=c.fnv(raw))
        sectors.append((at, source))
    ordered = sorted(sectors)
    m.require(len(set(at for at, _ in ordered)) == len(ordered), 'Sectores KC15 repetidos')
    for i, (at, source) in enumerate(ordered):
        stop = ordered[i + 1][0] if i + 1 < len(ordered) else len(data)
        m.require(at + rows[source]['size'] <= stop, 'Documentos KC15 solapados')
        rows[source]['allocation'] = stop - at
    hashes = []
    for i in range(h[4]):
        key, index, size, check = struct.unpack_from('<4I', data, h[8] * 512 + i * 16)
        # Ambiguous signatures legitimately use index 0xffffffff. Preserve
        # that decision; a revised target may not belong to such an alias.
        m.require(index < h[3] or index == 0xffffffff, 'Índice hash KC15 inválido')
        hashes.append((key, index, size, check))
    return h, rows, hashes


def export_bytes(conn, base_path, game='bprj1'):
    """Read-only; return the complete companion and an auditable change list."""
    m = translations.api(); c = codec(); base_path = Path(base_path)
    base = base_path.read_bytes()
    m.require(game in BASE_KC15_SHA256 and m.digest(base) == BASE_KC15_SHA256[game],
              'El KC15 base no es la identidad adoptada para este juego')
    h, rows, hashes = parse(base)
    g = conn.execute('SELECT * FROM koto_games WHERE game_id=?', (game,)).fetchone()
    m.require(g is not None, 'Juego desconocido')
    kh = m.kl.H.unpack_from(g['base_payload'])
    m.require(h[10] == kh[10] and h[11] == kh[11], 'KC15 no corresponde a la base KL29 inmutable')
    raw_counts = Counter(row['raw'] for row in rows.values())
    hash_counts = Counter(row['signature'] for row in rows.values())
    out = bytearray(base); changes = []; reviewed = 0
    if translations.present(conn):
        documents = conn.execute('SELECT d.* FROM koto_documents d WHERE game_id=? AND '
            'EXISTS (SELECT 1 FROM koto_document_translations t WHERE t.document_id=d.document_id) '
            'ORDER BY source_offset', (game,))
    else:
        documents = []
    for document in documents:
        state = translations.state(conn, document); value = state['value']
        translations.validate_value(document, value); reviewed += 1
        if value == translations.original(document):
            continue
        source = document['source_offset']; row = rows.get(source)
        m.require(row is not None, 'La traducción revisada carece de destino KC15')
        q = row['header']; break_count = (q[5] >> 8) & 255
        m.require(row['owner'] == row['page'] == 0 and q[5] == break_count << 8 and
                  not translations.original(document)['fragments'] and not value['fragments'],
                  'KC15 v1 sólo exporta documentos simples sin alias ni fragmentos')
        m.require(not translations.TOKEN.search(value['spanish']) and
                  not any(x in document['raw_source'] for x in (0xfd, 0xf7)),
                  'KC15 v1 requiere una revisión sin variables dinámicas')
        m.require(row['end'] == document['source_end'] and row['raw'] == c.normalize(document['raw_source']) and
                  row['spanish'] == document['spanish'], 'Preimagen KC15 distinta de la fuente lingüística')
        m.require(row['size'] == 32 + q[3] + q[4] + 1 + 2 * break_count,
                  'KC15 contiene metadatos auxiliares no admitidos por este exportador')
        m.require(raw_counts[row['raw']] == hash_counts[row['signature']] == 1,
                  'La firma KC15 revisada tiene alias o una colisión')
        routes = [r for r in hashes if r[0] == row['signature']]
        m.require(routes == [(row['signature'], row['index'], len(row['raw']), row['signature'])],
                  'Destino hash KC15 ambiguo o incompatible')
        encoded = value['spanish'].encode('utf-8'); breaks = c.spanish_breaks(value['spanish'])
        m.require(len(encoded) <= 1024 and len(breaks) <= 255, 'Castellano fuera de la capacidad KC15')
        m.require(all(0 <= n < len(encoded) and encoded[n] == 32 for n in breaks), 'Salto KC15 fuera de un espacio')
        body = row['raw'] + encoded + b'\0' + b''.join(struct.pack('<H', n) for n in breaks)
        new = D.pack(b'KOTODO15', c.fnv(body), source, len(row['raw']), len(encoded), len(breaks) << 8, len(body)) + body
        m.require(len(new) <= 1536 and len(new) <= row['allocation'], 'La revisión necesita nuevos sectores KC15')
        at = row['at']; stop = at + row['allocation']
        m.require(not any(base[at + row['size']:stop]), 'Relleno KC15 no vacío: no se puede sobrescribir')
        out[at:stop] = new + bytes(row['allocation'] - len(new))
        changes.append(dict(source_offset=source, document_id=document['document_id'],
            translation_revision=state['revision'], translation_sha256=state['sha256'], packet_id=state['packet_id'],
            index=row['index'], sector=row['sector'], allocation_bytes=row['allocation'],
            before_sha256=m.digest(row['doc']), after_sha256=m.digest(new),
            before_spanish=row['spanish'], after_spanish=value['spanish'],
            before_bytes=row['size'], after_bytes=len(new), spanish_breaks=breaks,
            raw_signature_sha256=m.digest(row['raw']), raw_signature_fnv=row['signature']))
    if changes:
        struct.pack_into('<I', out, 48, c.fnv(out[512:]))
    result = bytes(out); final_h, final_rows, final_hashes = parse(result)
    expected = {item['source_offset']: item for item in changes}
    m.require(final_h[:11] == h[:11] and final_h[12:] == h[12:] and final_hashes == hashes,
              'La exportación alteró identidades o directorios KC15')
    m.require(base[512:h[9] * 512] == result[512:h[9] * 512], 'Índices KC15 modificados')
    for source, row in rows.items():
        after = final_rows[source]
        m.require(all(row[k] == after[k] for k in ('end', 'sector', 'owner', 'page', 'index', 'raw')),
                  'La exportación alteró la identidad de un documento KC15')
        if source not in expected:
            m.require(row['doc'] == after['doc'], 'Documento KC15 ajeno a la revisión modificado')
        else:
            m.require(after['spanish'] == expected[source]['after_spanish'], 'Traducción KC15 no publicada')
    report = dict(schema='kotogba.memory_kc15_export/1', game_id=game,
        base_sha256=m.digest(base), output_sha256=m.digest(result), output_bytes=len(result),
        base_checksum=h[11], output_checksum=final_h[11], ks5_binding=h[10],
        documents=len(rows), hash_entries=len(hashes), reviewed_documents=reviewed,
        changed_documents=changes, unchanged_documents=len(rows) - len(changes),
        indices_owners_pages_signatures_unchanged=True, byte_identical_to_input=result == base,
        personal_collections_opened=False,
        supported_scope='Documentos simples sin variables, fragmentos, alias ni ampliación de sectores; cualquier otro cambio se rechaza antes de publicar.',
        codec_path=str(Path(c.__file__).resolve().relative_to(m.PROJECT)), codec_sha256=m.file_sha(Path(c.__file__)))
    return result, report
