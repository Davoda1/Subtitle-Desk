#!/usr/bin/env python3
"""Stage a native app with the pinned, redistributable recognition/Qt runtime.

The runtime reference is an extracted, checksum-verified Speech Desk 1.1.1
package. No application executable or documentation from that app is copied.
"""
import argparse, hashlib, json, os, re, shutil, subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
MODELS=('whistle.cact','ggml-tiny-q5_1.bin','ggml-base-q5_1.bin','ggml-small-q5_1.bin')
GUIDES=('README.md','README.html','Notes.txt','Verification.md','BUILDING.md')

def copy(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(src,dst)

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
    return h.hexdigest()

def stage_documentation(output, source_root=ROOT):
    """Always stage current application notices and offline package guides."""
    output.mkdir(parents=True, exist_ok=True)
    copy(source_root/'LICENSE', output/'LICENSE.txt')
    copy(source_root/'NOTICE', output/'NOTICE.txt')
    for filename in GUIDES:
        copy(source_root/'docs'/filename, output/filename)
    copy(source_root/'SECURITY.md', output/'SECURITY.md')
    changelog=(source_root/'CHANGELOG.md').read_text(encoding='utf-8')
    for filename in ('README.md', 'Verification.md'):
        # These guides are beside CHANGELOG.md in a portable package. Keep
        # package navigation usable offline without changing repository links.
        for target in (f'docs/{filename}',
                       f'https://github.com/Davoda1/Subtitle-Desk/blob/master/docs/{filename}'):
            changelog=changelog.replace(f']({target})', f']({filename})')
    (output/'CHANGELOG.md').write_text(changelog, encoding='utf-8')

def stage(args):
    out=args.output.resolve();ref=args.runtime_reference.resolve();out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()):raise SystemExit('Output must be an empty directory, to avoid stale package content.')
    for folder in ('plugins','licenses','resources'):
        shutil.copytree(ref/folder,out/folder,ignore=shutil.ignore_patterns('.*'))
    lock=json.loads((ROOT/'vendor/runtime-lock.json').read_text())
    for entry in lock['models']:
        path=out/'resources/models'/entry['file']
        if path.stat().st_size!=entry['size'] or sha(path)!=entry['sha256']:
            raise SystemExit('Bundled model validation failed: '+entry['file'])
    copy(ROOT/'vendor/runtime-lock.json',out/'runtime-lock.json')
    stage_documentation(out)
    for notice in (ROOT/'vendor/licenses').iterdir():
        if notice.is_file() and not notice.name.startswith('.'):copy(notice,out/'licenses'/notice.name)
    for filename in ('README.txt','COPYING.LGPLv2.1','provenance.json'):
        src=ROOT/'vendor/ffmpeg'/filename
        if src.exists():copy(src,out/'licenses'/('FFmpeg-Headers-'+filename))
    if args.platform=='windows':
        copy(args.binary,out/'SubtitleDesk.exe');os.chmod(out/'SubtitleDesk.exe',0o755)
        for dll in ref.glob('*.dll'):
            if not dll.name.startswith('.'):copy(dll,out/dll.name)
        if args.compiler:
            for name in ('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll'):
                src=Path(subprocess.check_output([args.compiler,'-print-file-name='+name],text=True).strip())
                if not src.is_file():raise SystemExit('Compiler runtime not found: '+name)
                copy(src,out/name);os.chmod(out/name,0o755)
        (out/'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=plugins\n',encoding='utf-8')
    else:
        copy(args.binary,out/'subtitledesk');os.chmod(out/'subtitledesk',0o755)
        shutil.copytree(ref/'lib',out/'lib',ignore=shutil.ignore_patterns('.*'))
        subprocess.run([args.patchelf,'--set-rpath','$ORIGIN/lib:$ORIGIN/../lib/subtitledesk/lib',str(out/'subtitledesk')],check=True)
        for p in (out/'lib').glob('*.so*'):
            if p.is_file():subprocess.run([args.patchelf,'--set-rpath','$ORIGIN',str(p)],check=True)
        for p in (out/'plugins').rglob('*.so'):
            subprocess.run([args.patchelf,'--set-rpath','$ORIGIN/../../lib',str(p)],check=True)
        for p in (out/'resources/bin').iterdir():
            if p.is_file():os.chmod(p,0o755)
        (out/'qt.conf').write_text('[Paths]\nPrefix=.\nLibraries=lib\nPlugins=plugins\n',encoding='utf-8')
    print(json.dumps({'platform':args.platform,'staged':str(out),'models_verified':len(MODELS),'size_bytes':sum(p.stat().st_size for p in out.rglob('*') if p.is_file())},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--platform',choices=('windows','linux'),required=True)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--runtime-reference',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--compiler');p.add_argument('--patchelf',default='patchelf')
    stage(p.parse_args())
