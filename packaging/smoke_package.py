#!/usr/bin/env python3
"""Launch the staged application without a Qt SDK environment and capture its UI."""

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path, PureWindowsPath


def isolated_environment(platform, source):
    # os.environ is case-insensitive on Windows, but dict copies are not.
    # Normalize before looking up or removing entries in the child environment.
    environment = {key.upper(): value for key, value in source.items()} if platform == 'windows' else dict(source)
    for key in ['QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH',
                'QT_ROOT_DIR', 'QTDIR', 'LD_LIBRARY_PATH', 'SUBTITLEDESK_RESOURCES']:
        environment.pop(key, None)
    environment['QT_QPA_PLATFORM'] = 'offscreen'
    environment['QT_MEDIA_BACKEND'] = 'ffmpeg'
    if platform == 'windows':
        system_root = environment.get('SYSTEMROOT') or environment.get('WINDIR')
        if not system_root:
            raise ValueError('The Windows environment must provide SYSTEMROOT or WINDIR.')
        environment['SYSTEMROOT'] = system_root
        windows = PureWindowsPath(system_root)
        environment['PATH'] = ';'.join([str(windows / 'System32'), str(windows)])
    return environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['linux', 'windows'], required=True)
    parser.add_argument('--path', type=Path, required=True)
    parser.add_argument('--screenshot', type=Path, required=True)
    args = parser.parse_args()
    binary = args.path.resolve() / ('SubtitleDesk.exe' if args.platform == 'windows' else 'subtitledesk')
    screenshot = args.screenshot.resolve()
    screenshot.parent.mkdir(parents=True, exist_ok=True)
    environment = isolated_environment(args.platform, os.environ)
    with tempfile.TemporaryDirectory(prefix='subtitledesk-package-test-') as temporary:
        subprocess.run([str(binary), '--demo', '--theme', 'dark', '--size', '1180x800',
                        '--screenshot', str(screenshot)], cwd=temporary, env=environment,
                       check=True, timeout=30)
    data = screenshot.read_bytes()
    if len(data) < 10000 or data[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack('>II', data[16:24]) != (1180, 800):
        raise SystemExit('The native package did not produce the expected application screenshot.')
    print('Native package started successfully and rendered the complete editor.')


if __name__ == '__main__':
    main()
