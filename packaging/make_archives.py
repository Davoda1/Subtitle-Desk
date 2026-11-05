#!/usr/bin/env python3
"""Create release archives without copying build outputs into the source."""

import argparse
import concurrent.futures
import subprocess
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT_FILES = {
    '.gitignore', '.gitattributes', '.editorconfig', 'CMakeLists.txt',
    'LICENSE', 'NOTICE', 'README.md', 'CHANGELOG.md', 'CONTRIBUTING.md',
    'SECURITY.md', 'CODE_OF_CONDUCT.md',
}
SOURCE_DIRECTORIES = {'.github', 'src', 'tests', 'resources', 'vendor', 'packaging', 'docs'}


def eligible_source_path(relative):
    """A source archive extracted without .git still has a bounded file set."""
    parts = relative.parts
    if not parts or not (parts[0] in SOURCE_ROOT_FILES or parts[0] in SOURCE_DIRECTORIES):
        return False
    if any(part in {'__pycache__', 'dist', '.git'} or part.startswith('build') for part in parts):
        return False
    if any(part.startswith('.') and part not in {'.github', '.gitignore', '.gitattributes', '.editorconfig'} for part in parts):
        return False
    if relative.suffix in {'.pyc', '.pyo', '.user', '.sdesk'}:
        return False
    if len(parts) > 1 and parts[0] == 'resources' and parts[1] in {'models', 'bin', 'lib'}:
        return False
    return True


def source_files(root):
    """Use only tracked source in a checkout, or the documented source tree."""
    root = root.resolve()
    if (root / '.git').exists():
        result = subprocess.run(
            ['git', '-C', str(root), 'ls-files', '-z', '--cached'],
            check=True, capture_output=True,
        )
        candidates = [root / name.decode('utf-8') for name in result.stdout.split(b'\0') if name]
    else:
        candidates = list(root.rglob('*'))
    files = []
    for path in candidates:
        relative = path.relative_to(root)
        if not eligible_source_path(relative):
            continue
        if path.is_symlink():
            raise ValueError(f'Source archives cannot follow symlinks: {relative}')
        if path.is_file():
            files.append(path)
    return sorted(files)


def runtime_files(root):
    return [path for path in sorted(root.rglob('*'))
            if path.is_file() and not any(part.startswith('.') or part == '__pycache__'
                                         for part in path.relative_to(root).parts)
            and path.suffix not in {'.pyc', '.pyo'}]


def archive(root, output, kind, prefix='', source=False):
    # source_files() returns resolved paths. Use the same spelling here for
    # relative member names, including Windows short paths and directory aliases.
    root = root.resolve()
    paths = source_files(root) if source else runtime_files(root)
    if source and not paths:
        raise ValueError('No source files selected. Stage the intended files in Git before creating a source release.')
    output.parent.mkdir(parents=True, exist_ok=True)
    if kind == 'zip':
        with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for path in paths:
                bundle.write(path, (Path(prefix) / path.relative_to(root)).as_posix())
    else:
        with tarfile.open(output, 'w:gz', compresslevel=6) as bundle:
            for path in paths:
                # Archive ownership must never embed a local builder's identity.
                info = bundle.gettarinfo(str(path), arcname=(Path(prefix) / path.relative_to(root)).as_posix())
                info.uid = info.gid = 0
                info.uname = info.gname = ''
                with path.open('rb') as content:
                    bundle.addfile(info, content)
    print(output.name, output.stat().st_size, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux', type=Path, required=True)
    parser.add_argument('--windows', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    jobs = [
        (args.windows, args.output / 'SubtitleDesk-1.0.0-Windows-x64.zip', 'zip', '', False),
        (args.linux, args.output / 'SubtitleDesk-1.0.0-Linux-amd64.tar.gz', 'tar', '', False),
        (ROOT, args.output / 'SubtitleDesk-1.0.0-Source.tar.gz', 'tar', 'SubtitleDesk-1.0.0-Source', True),
        (ROOT, args.output / 'SubtitleDesk-1.0.0-Source.zip', 'zip', 'SubtitleDesk-1.0.0-Source', True),
    ]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for task in [pool.submit(archive, *job) for job in jobs]:
            task.result()


if __name__ == '__main__':
    main()
