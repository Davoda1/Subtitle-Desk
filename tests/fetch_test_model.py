#!/usr/bin/env python3
"""Explicitly acquire the pinned optional model used by import integration tests."""

# Copyright (c) 2026 davoda1
# SPDX-License-Identifier: AGPL-3.0-only

import argparse
import hashlib
import os
import tempfile
import urllib.request
from pathlib import Path


NAME = 'ggml-tiny.en-q5_1.bin'
URL = ('https://huggingface.co/ggerganov/whisper.cpp/resolve/'
       '5359861c739e955e79d9a303bcbc70fb988958b1/' + NAME)
SIZE = 32_166_155
SHA256 = 'c77c5766f1cef09b6b7d47f21b546cbddd4157886b3b5d6d4f709e91e66c7c2b'


def matches(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != SIZE:
        return False
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest() == SHA256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=True)
    target = args.destination / NAME
    if target.is_symlink():
        raise SystemExit('Refusing a symlink at the model destination.')
    if matches(target):
        print(f'Verified existing test model: {target}')
        return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix='test-model-', suffix='.tmp',
                                         dir=args.destination, delete=False) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(URL, headers={'User-Agent': 'SubtitleDesk-test-fixture/1.1.0'})
            with urllib.request.urlopen(request, timeout=90) as response:
                received = 0
                while chunk := response.read(1024 * 1024):
                    received += len(chunk)
                    if received > SIZE:
                        raise ValueError('Downloaded test model exceeds the pinned size.')
                    output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        if not matches(temporary):
            raise ValueError('Test model size or SHA-256 mismatch; nothing was installed.')
        temporary.replace(target)
        print(f'Downloaded and verified test model: {target}')
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
