"""Regression tests for release source boundaries and native package checks."""

import importlib.util
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'packaging' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


archives = load_module('make_archives')
packages = load_module('validate_package')
smoke = load_module('smoke_package')
staging = load_module('package')


class SourceBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = (Path(self.temporary.name) / 'source').resolve()
        self.root.mkdir()

    def put(self, name, data=b'fixture'):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def test_source_archives_include_workflows_but_exclude_runtime_and_local_files(self):
        wanted = ['CMakeLists.txt', 'README.md', '.gitignore', '.github/workflows/ci.yml',
                  'src/main.cpp', 'docs/images/editor.png', 'tests/fixtures/speech.wav',
                  'vendor/runtime-lock.json']
        rejected = ['build/CMakeCache.txt', 'build-assets/linux/resources/models/model.bin',
                    'dist/release.zip', 'notes-private.txt', '.env', '.git/config',
                    'resources/models/model.bin', 'resources/bin/engine',
                    'tests/__pycache__/test.pyc', 'docs/.cache/session.json', 'docs/recovery.sdesk']
        for name in wanted + rejected:
            self.put(name)
        # This fixture represents an extracted source archive, not a checkout.
        shutil.rmtree(self.root / '.git')
        self.assertEqual({p.relative_to(self.root).as_posix() for p in archives.source_files(self.root)}, set(wanted))
        for kind in ['zip', 'tar']:
            output = self.root.parent / ('source.' + kind)
            archives.archive(self.root, output, kind, 'SubtitleDesk-Source', source=True)
            if kind == 'zip':
                with zipfile.ZipFile(output) as bundle:
                    names = bundle.namelist()
            else:
                with tarfile.open(output) as bundle:
                    members = bundle.getmembers()
                    names = [member.name for member in members]
                    self.assertTrue(all(member.uid == member.gid == 0 for member in members))
                    self.assertTrue(all(member.uname == member.gname == '' for member in members))
            self.assertEqual(set(names), {'SubtitleDesk-Source/' + name for name in wanted})

    @unittest.skipUnless(shutil.which('git'), 'Git is needed to exercise tracked-file selection.')
    def test_checkout_only_archives_tracked_source(self):
        self.put('README.md')
        self.put('src/main.cpp')
        self.put('docs/untracked.md', b'local draft')
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        subprocess.run(['git', '-C', str(self.root), 'add', 'README.md', 'src/main.cpp'], check=True)
        self.assertEqual({p.relative_to(self.root).as_posix() for p in archives.source_files(self.root)}, {'README.md', 'src/main.cpp'})

    def test_archive_normalizes_noncanonical_source_root(self):
        self.put('README.md')
        self.put('.github/workflows/ci.yml')
        # A path with '..' exercises normalization on every platform without
        # requiring permission to create symlinks or Windows short filenames.
        alias = self.root / '..' / self.root.name
        for kind in ['zip', 'tar']:
            with self.subTest(kind=kind):
                output = self.root.parent / ('aliased-source.' + kind)
                archives.archive(alias, output, kind, 'Source', source=True)
                if kind == 'zip':
                    with zipfile.ZipFile(output) as bundle:
                        names = bundle.namelist()
                else:
                    with tarfile.open(output) as bundle:
                        names = bundle.getnames()
                self.assertEqual(set(names), {'Source/README.md', 'Source/.github/workflows/ci.yml'})

    def test_source_symlinks_are_rejected(self):
        self.put('README.md')
        outside = self.root.parent / 'outside.txt'
        outside.write_text('outside source tree', encoding='utf-8')
        link = self.root / 'docs/link.txt'
        link.parent.mkdir()
        try:
            link.symlink_to(outside)
        except OSError:
            self.skipTest('This account cannot create filesystem symlinks.')
        with self.assertRaisesRegex(ValueError, 'symlinks'):
            archives.source_files(self.root)


class NativeBinaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'binary'

    def test_launchers_and_wrong_architectures_are_rejected(self):
        for content in [b'#!/bin/sh\nexec another-program\n', b'not a binary', b'MZ' + bytes(62)]:
            self.path.write_bytes(content)
            os.chmod(self.path, 0o755)
            for platform in ['linux', 'windows']:
                with self.subTest(platform=platform, content=content[:8]):
                    with self.assertRaises(ValueError):
                        packages.native_binary(self.path, platform)

    def test_native_x64_headers_are_accepted(self):
        elf = bytearray(64)
        elf[:6] = b'\x7fELF\x02\x01'
        elf[18:20] = (62).to_bytes(2, 'little')
        self.path.write_bytes(elf)
        os.chmod(self.path, 0o755)
        packages.native_binary(self.path, 'linux')
        pe = bytearray(70)
        pe[:2] = b'MZ'
        pe[60:64] = (64).to_bytes(4, 'little')
        pe[64:70] = b'PE\0\0\x64\x86'
        self.path.write_bytes(pe)
        packages.native_binary(self.path, 'windows')


class PackageEnvironmentTests(unittest.TestCase):
    def test_windows_environment_keys_are_case_insensitive(self):
        for root_key in ['SYSTEMROOT', 'SystemRoot', 'sYsTeMrOoT', 'windir']:
            with self.subTest(root_key=root_key):
                source = {root_key: r'C:\Windows', 'Path': r'D:\Qt\bin;D:\Compiler\bin',
                          'qt_plugin_path': r'D:\Qt\plugins', 'SubtitleDesk_Resources': r'D:\models'}
                original = dict(source)
                environment = smoke.isolated_environment('windows', source)
                self.assertEqual(environment['SYSTEMROOT'], r'C:\Windows')
                self.assertEqual(environment['PATH'], r'C:\Windows\System32;C:\Windows')
                self.assertNotIn('QT_PLUGIN_PATH', environment)
                self.assertNotIn('SUBTITLEDESK_RESOURCES', environment)
                self.assertEqual(environment['QT_QPA_PLATFORM'], 'offscreen')
                self.assertEqual(source, original)


class PackageDocumentationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'source'
        self.output = Path(self.temporary.name) / 'package'
        (self.root / 'docs').mkdir(parents=True)
        self.output.mkdir()
        (self.root / 'LICENSE').write_text('GNU AFFERO GENERAL PUBLIC LICENSE\nVersion 3\n', encoding='utf-8')
        (self.root / 'NOTICE').write_text('Copyright (c) 2026 davoda1\nAGPL-3.0-only\n', encoding='utf-8')
        (self.root / 'SECURITY.md').write_text('Public security policy\n', encoding='utf-8')
        for name in ['README.md', 'README.html', 'Notes.txt', 'Verification.md', 'BUILDING.md']:
            (self.root / 'docs' / name).write_text('Current guide: ' + name + '\n', encoding='utf-8')

    def test_current_notices_and_notes_replace_stale_files_with_local_guide_links(self):
        for links in ['docs/', 'https://github.com/Davoda1/Subtitle-Desk/blob/master/docs/']:
            with self.subTest(link_prefix=links):
                (self.root / 'CHANGELOG.md').write_text(
                    f'[User guide]({links}README.md) and [Verification]({links}Verification.md)\n',
                    encoding='utf-8')
                (self.output / 'Notes.txt').write_text('Stale package notes\n', encoding='utf-8')
                third_party = self.output / 'licenses' / 'dependency.txt'
                third_party.parent.mkdir(exist_ok=True)
                third_party.write_bytes(b'Original dependency attribution\n')
                staging.stage_documentation(self.output, self.root)
                self.assertEqual((self.output / 'LICENSE.txt').read_bytes(), (self.root / 'LICENSE').read_bytes())
                self.assertEqual((self.output / 'NOTICE.txt').read_bytes(), (self.root / 'NOTICE').read_bytes())
                self.assertEqual((self.output / 'Notes.txt').read_bytes(), (self.root / 'docs/Notes.txt').read_bytes())
                self.assertEqual((self.output / 'CHANGELOG.md').read_text(encoding='utf-8'),
                                 '[User guide](README.md) and [Verification](Verification.md)\n')
                self.assertEqual(third_party.read_bytes(), b'Original dependency attribution\n')
                packages.validate_documentation(self.output, self.root)

    def test_missing_or_stale_application_notices_are_rejected(self):
        (self.root / 'CHANGELOG.md').write_text(
            '[User guide](docs/README.md) and [Verification](docs/Verification.md)\n', encoding='utf-8')
        for name in ['LICENSE.txt', 'NOTICE.txt', 'Notes.txt']:
            with self.subTest(filename=name):
                staging.stage_documentation(self.output, self.root)
                (self.output / name).write_text('Outdated contents\n', encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'outdated'):
                    packages.validate_documentation(self.output, self.root)
                (self.output / name).unlink()
                with self.assertRaisesRegex(ValueError, 'missing'):
                    packages.validate_documentation(self.output, self.root)


if __name__ == '__main__':
    unittest.main()
