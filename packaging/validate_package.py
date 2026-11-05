#!/usr/bin/env python3
"""Validate a staged native package and the hashes of its four local models."""

import argparse
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def native_binary(path, platform):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'A real native executable is required: {path.name}')
    with path.open('rb') as stream:
        header = stream.read(64)
        if platform == 'linux':
            if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 62:
                raise ValueError(f'Expected a 64-bit x86 Linux executable: {path.name}')
            if not os.access(path, os.X_OK):
                raise ValueError(f'Missing executable mode: {path.name}')
        else:
            if header[:2] != b'MZ' or len(header) != 64:
                raise ValueError(f'Expected a Windows executable: {path.name}')
            stream.seek(int.from_bytes(header[60:64], 'little'))
            pe = stream.read(6)
            if pe != b'PE\0\0\x64\x86':
                raise ValueError(f'Expected a 64-bit x86 Windows executable: {path.name}')


def validate_documentation(path, source_root=ROOT):
    """Reject missing or stale first-party license, ownership, and guides."""
    sources = {'LICENSE.txt': source_root / 'LICENSE',
               'NOTICE.txt': source_root / 'NOTICE',
               'SECURITY.md': source_root / 'SECURITY.md'}
    for name in ['README.md', 'README.html', 'Notes.txt', 'Verification.md', 'BUILDING.md']:
        sources[name] = source_root / 'docs' / name
    for name, source in sources.items():
        packaged = path / name
        if not packaged.is_file():
            raise ValueError(f'Package is missing {name}')
        if packaged.read_bytes() != source.read_bytes():
            raise ValueError(f'Package has outdated application documentation: {name}')
    changelog = path / 'CHANGELOG.md'
    if not changelog.is_file():
        raise ValueError('Package is missing CHANGELOG.md')
    text = changelog.read_text(encoding='utf-8')
    for name in ['README.md', 'Verification.md']:
        if f']({name})' not in text:
            raise ValueError(f'Packaged changelog must link to its local {name} guide.')


def validate(path, platform, runtime_only=False):
    if not runtime_only:
        native_binary(path / ('SubtitleDesk.exe' if platform == 'windows' else 'subtitledesk'), platform)
        validate_documentation(path)
        for filename in ['runtime-lock.json', 'qt.conf']:
            if not (path / filename).is_file():
                raise ValueError(f'Package is missing {filename}')
        for unwanted in ['SpeechDesk.exe', 'speechdesk', '.git', '.env', 'settings.json', 'recovery.sdesk']:
            if (path / unwanted).exists():
                raise ValueError(f'Unexpected package content: {unwanted}')
    lock = json.loads((ROOT / 'vendor/runtime-lock.json').read_text(encoding='utf-8'))
    for model in lock['models']:
        file = path / 'resources/models' / model['file']
        if not file.is_file() or file.stat().st_size != model['size'] or sha256(file) != model['sha256']:
            raise ValueError(f'Model does not match the dependency lock: {model["file"]}')
    for engine in ['needle', 'whisper-cli']:
        native_binary(path / 'resources/bin' / (engine + ('.exe' if platform == 'windows' else '')), platform)
    for name in ['Whistle-Apache-2.0.txt', 'OpenAI-Whisper-MIT.txt', 'whisper.cpp-MIT.txt',
                 'Needle-Apache-2.0.txt', 'Qt-LGPL-3.0.txt', 'FFmpeg-LGPL-2.1.txt']:
        if not (path / 'licenses' / name).is_file():
            raise ValueError(f'Package is missing a dependency notice: {name}')
    libraries = path if platform == 'windows' else path / 'lib'
    for name, version in [('avcodec', 61), ('avformat', 61), ('avutil', 59), ('swresample', 5)]:
        filename = f'{name}-{version}.dll' if platform == 'windows' else f'lib{name}.so.{version}'
        if not (libraries / filename).is_file():
            raise ValueError(f'Package is missing the pinned decoder: {filename}')
    if not (path / 'plugins/multimedia').is_dir():
        raise ValueError('Package is missing the Qt multimedia plugin directory.')
    return len(lock['models'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--path', type=Path, required=True)
    parser.add_argument('--platform', choices=['linux', 'windows'], required=True)
    parser.add_argument('--runtime-only', action='store_true', help='Validate a runtime donor before application staging.')
    args = parser.parse_args()
    count = validate(args.path.resolve(), args.platform, args.runtime_only)
    print(f'Validated {args.platform} package: {count} pinned models, native engines, decoder, and license notices.')


if __name__ == '__main__':
    main()
