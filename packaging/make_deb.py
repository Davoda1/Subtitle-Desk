#!/usr/bin/env python3
"""Build a Debian package with a real /usr/bin/subtitledesk executable."""
import argparse, os, shutil, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--portable', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='subtitledesk-deb-') as td:
    root = Path(td)
    runtime = root / 'usr/lib/subtitledesk'
    runtime.parent.mkdir(parents=True)
    shutil.copytree(a.portable, runtime, ignore=shutil.ignore_patterns('.*'))
    binary = root / 'usr/bin/subtitledesk'
    binary.parent.mkdir(parents=True)
    shutil.move(str(runtime / 'subtitledesk'), binary)
    os.chmod(binary, 0o755)
    # The executable selects private plugins; no shared /usr/bin/qt.conf.
    desktop = root / 'usr/share/applications/subtitledesk.desktop'
    desktop.parent.mkdir(parents=True)
    desktop.write_text('[Desktop Entry]\nType=Application\nName=Subtitle Desk\nComment=Offline subtitle and lyrics editor\nExec=subtitledesk %f\nIcon=subtitledesk\nTerminal=false\nCategories=AudioVideo;Audio;AudioVideoEditing;\nMimeType=audio/x-wav;audio/mpeg;video/mp4;video/x-matroska;application/x-subrip;text/vtt;\n', encoding='utf-8')
    icon = root / 'usr/share/icons/hicolor/256x256/apps/subtitledesk.png'
    icon.parent.mkdir(parents=True)
    shutil.copy2(ROOT / 'resources/app.png', icon)
    docs = root / 'usr/share/doc/subtitledesk'
    docs.mkdir(parents=True)
    notice = (ROOT / 'NOTICE').read_text(encoding='utf-8')
    license_text = (ROOT / 'LICENSE').read_text(encoding='utf-8')
    (docs / 'copyright').write_text(notice.rstrip() + '\n\n' + license_text,
                                   encoding='utf-8')
    shutil.copy2(ROOT / 'NOTICE', docs / 'NOTICE.txt')
    for name in ('README.md', 'README.html', 'Notes.txt', 'Verification.md',
                 'BUILDING.md', 'CHANGELOG.md', 'SECURITY.md'):
        shutil.copy2(runtime / name, docs / name)
    control = root / 'DEBIAN'
    control.mkdir()
    size = sum(f.stat().st_size for f in root.rglob('*') if f.is_file()) // 1024
    (control / 'control').write_text(f'''Package: subtitledesk
Version: 1.1.0
Section: sound
Priority: optional
Architecture: amd64
Maintainer: davoda1
Installed-Size: {size}
Depends: libc6 (>= 2.34), libstdc++6 (>= 11), libgcc-s1, libgomp1, libasound2 | libasound2t64, libpulse0, libfontconfig1, libfreetype6, libglib2.0-0 | libglib2.0-0t64, libdbus-1-3, libgl1, libegl1, libopengl0, libx11-6, libx11-xcb1, libxext6, libxrender1, libxrandr2, libxcb1, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-shm0, libxcb-sync1, libxcb-util1, libxcb-xfixes0, libxcb-xkb1, libxkbcommon0, libxkbcommon-x11-0, libssl3 | libssl3t64, zlib1g
Description: Offline subtitle and lyrics editor
 Native audio-only transcription and editing with four bundled CPU speech
 recognition models and import of compatible local Whisper/Whistle models.
 Includes synchronized cue highlighting, waveform navigation, review tools,
 and SRT, VTT, ASS, LRC, TXT, CSV and JSON exports.
''', encoding='utf-8')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['dpkg-deb', '--root-owner-group', '-Zxz', '-z3', '--build', str(root), str(a.output.resolve())], check=True)
