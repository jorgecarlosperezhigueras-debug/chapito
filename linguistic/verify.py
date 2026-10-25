#!/usr/bin/env python3
"""Verify the public B12 code and, optionally, its private recovery inputs.

Read only: never restores, imports, exports, uploads, or changes a database.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def safe_path(root: Path, relative: str) -> Path:
    path = Path(relative)
    if path.is_absolute() or not relative or '..' in path.parts:
        raise ValueError('Ruta relativa inválida en el manifiesto')
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root.resolve()):
        raise ValueError('La dependencia sale de su raíz')
    if not resolved.is_file():
        raise ValueError('Falta la dependencia: ' + relative)
    return resolved


def file_sha(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def match(path: Path, record: dict) -> None:
    if path.stat().st_size != record['bytes'] or file_sha(path) != record['sha256']:
        raise ValueError('Bytes distintos de la referencia B12: ' + record['path'])


def verify(private_project: Path | None = None) -> dict:
    manifest = json.loads((HERE / 'SOURCES.json').read_text('utf-8'))
    if manifest.get('schema') != 'kotogba.linguistic-sources/1':
        raise ValueError('Esquema de fuentes desconocido')
    if (manifest['memory_revision'], manifest['runtime_revision'], manifest['installable']) != (50, 36, False):
        raise ValueError('La referencia debe conservar B12 50/36 no instalable')
    seen = set()
    for item in manifest['files']:
        if item['tracked_path'] in seen:
            raise ValueError('Fuente repetida en el manifiesto')
        seen.add(item['tracked_path'])
        if item['original_sha256'] != item['sha256']:
            raise ValueError('Esta instantánea exige conservar los bytes originales')
        path = safe_path(HERE, item['tracked_path'])
        match(path, item)
        if path.suffix == '.py':
            compile(path.read_bytes(), item['tracked_path'], 'exec')
    result = {'schema': 'kotogba.linguistic-code-check/1', 'release': 'B12',
              'memory_revision': 50, 'runtime_revision': 36, 'installable': False,
              'public_code_files_verified': len(seen), 'public_code_bytes': sum(x['bytes'] for x in manifest['files']),
              'private_inputs_verified': False, 'private_input_count': 0, 'passed': True}
    if private_project is not None:
        project = private_project.resolve()
        if not project.is_dir():
            raise ValueError('No existe la raíz del proyecto privado recuperado')
        roots = {'project': project, 'recovery': project.parent}
        private = json.loads((HERE / 'PRIVATE_INPUTS.json').read_text('utf-8'))
        if private.get('schema') != 'kotogba.linguistic-private-inputs/1':
            raise ValueError('Esquema de dependencias privadas desconocido')
        for item in manifest['files'] + private['files']:
            if item['root'] not in roots:
                raise ValueError('Raíz de dependencia desconocida')
            match(safe_path(roots[item['root']], item['path']), item)
        result['private_inputs_verified'] = True
        result['private_input_count'] = len(private['files'])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-project', type=Path,
                        help='Raíz recuperada kotoGBA_B12 o r38_trabajo; dictionary_data debe quedar a su lado')
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.private_project), ensure_ascii=False, sort_keys=True))
        return 0
    except (ValueError, OSError, KeyError, SyntaxError) as exc:
        print(json.dumps({'passed': False, 'error': str(exc)}, ensure_ascii=False))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
