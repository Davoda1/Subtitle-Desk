#!/usr/bin/env python3
"""Explicit build-time downloader for the pinned Qt/STT distribution.
The shipped desktop application never invokes this script or downloads models.
"""
import argparse,hashlib,json,tarfile,urllib.request,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('platform',choices=['linux','windows']);p.add_argument('destination',type=Path);a=p.parse_args()
entry=json.loads((ROOT/'vendor/runtime-lock.json').read_text())['distributions'][a.platform]
a.destination.mkdir(parents=True,exist_ok=True);archive=a.destination/entry['filename']
if not archive.exists():
    request=urllib.request.Request(entry['url'],headers={'User-Agent':'SubtitleDesk-build'})
    with urllib.request.urlopen(request,timeout=120) as src,archive.open('wb') as dst:
        while chunk:=src.read(1048576):dst.write(chunk)
h=hashlib.sha256()
with archive.open('rb') as f:
    while chunk:=f.read(1048576):h.update(chunk)
if h.hexdigest()!=entry['sha256']:raise SystemExit('Checksum mismatch. Remove the incomplete archive and retry.')
output=a.destination/a.platform
if output.exists():raise SystemExit('The extraction destination already exists; choose an empty destination.')
output.mkdir()
if a.platform=='linux':
    with tarfile.open(archive) as t:t.extractall(output,filter='data')
else:
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            target=(output/member.filename).resolve()
            if not target.is_relative_to(output.resolve()):raise SystemExit('Unsafe archive member rejected.')
        z.extractall(output)
print('Verified runtime extracted to',output)
