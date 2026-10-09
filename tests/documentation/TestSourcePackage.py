"""Reproducibility and content controls of scripts/package-source.py (#121)."""
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('package_source', ROOT / 'scripts/package-source.py')
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)
ENVIRONMENT = {'GIT_AUTHOR_NAME': 'test', 'GIT_AUTHOR_EMAIL': 'test@example.invalid',
               'GIT_COMMITTER_NAME': 'test', 'GIT_COMMITTER_EMAIL': 'test@example.invalid',
               'GIT_AUTHOR_DATE': '2026-10-09T00:00:00Z', 'GIT_COMMITTER_DATE': '2026-10-09T00:00:00Z',
               'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_SYSTEM': os.devnull, 'PATH': os.environ.get('PATH', '')}


def git(root, *arguments):
    return subprocess.run(['git', '-C', str(root), *arguments], check=True, capture_output=True,
                          env=ENVIRONMENT).stdout


@unittest.skipUnless(shutil.which('git'), 'git is required')
class SourcePackageTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.work = Path(self.directory.name)
        self.repository = self.work / 'repository'
        (self.repository / 'tests').mkdir(parents=True)
        (self.repository / 'CMakeLists.txt').write_text('project(mddlog VERSION 1.2.3 LANGUAGES CXX)\n')
        (self.repository / 'tests/CMakeLists.txt').write_text(
            'CPMAddPackage(\n    NAME SpecLab\n    GITHUB_REPOSITORY ambroise-leclerc/SpecLab\n'
            '    # SpecLab v0.4.0\n    GIT_TAG ' + 'a' * 40 + '\n)\n')
        script = self.repository / 'run.sh'
        script.write_text('#!/bin/sh\n')
        script.chmod(0o755)
        git(self.repository, 'init', '-q')
        git(self.repository, 'add', '-A')
        git(self.repository, 'commit', '-q', '-m', 'fixture')
        self.commit = git(self.repository, 'rev-parse', 'HEAD').decode().strip()

    def tearDown(self):
        self.directory.cleanup()

    def members(self, manifest, output):
        data = gzip.decompress((output / manifest['archive']['name']).read_bytes())
        self.assertEqual(hashlib.sha256(data).hexdigest(), manifest['archive']['tarSha256'])
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            return {member.name: member for member in archive.getmembers()}, data

    def test_commit_archive_is_reproducible_and_normalized(self):
        first = PACKAGE.package(self.repository, self.work / 'one')
        second = PACKAGE.package(self.repository, self.work / 'two')
        self.assertEqual(first, second)
        self.assertEqual((self.work / 'one' / first['archive']['name']).read_bytes(),
                         (self.work / 'two' / second['archive']['name']).read_bytes())
        members, data = self.members(first, self.work / 'one')
        self.assertEqual(sorted(members), [f'mddlog-1.2.3/{name}' for name in
                                           ('CMakeLists.txt', 'SOURCE_REVISION', 'run.sh', 'tests/CMakeLists.txt')])
        mtime = int(git(self.repository, 'show', '-s', '--format=%ct', 'HEAD'))
        for member in members.values():
            self.assertEqual((member.uid, member.gid, member.uname, member.gname, member.mtime), (0, 0, '', '', mtime))
        self.assertEqual(members['mddlog-1.2.3/run.sh'].mode, 0o755)
        self.assertEqual(members['mddlog-1.2.3/CMakeLists.txt'].mode, 0o644)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            revision = archive.extractfile('mddlog-1.2.3/SOURCE_REVISION').read().decode()
        self.assertEqual(revision, f'{self.commit}\ncommit\n')
        self.assertEqual(first['sourceRevision'], {'commit': self.commit, 'state': 'commit'})
        speclab = [entry for entry in first['dependencies'] if entry['name'] == 'SpecLab']
        self.assertEqual(speclab[0]['pin'], 'SpecLab v0.4.0 ' + 'a' * 40)
        recorded = (self.work / 'one' / f'{first["archive"]["name"]}.sha256').read_text().split()[0]
        self.assertEqual(recorded, first['archive']['sha256'])

    def test_commit_archive_ignores_uncommitted_changes(self):
        clean = PACKAGE.package(self.repository, self.work / 'clean')
        (self.repository / 'CMakeLists.txt').write_text('project(mddlog VERSION 1.2.4 LANGUAGES CXX)\n')
        self.assertEqual(PACKAGE.package(self.repository, self.work / 'again')['archive'], clean['archive'])
        worktree = PACKAGE.package(self.repository, self.work / 'worktree', worktree=True)
        self.assertEqual(worktree['version'], '1.2.4')
        self.assertEqual(worktree['sourceRevision']['state'], 'worktree')

    def test_source_date_epoch_overrides_commit_time(self):
        os.environ['SOURCE_DATE_EPOCH'] = '1000'
        try:
            manifest = PACKAGE.package(self.repository, self.work / 'epoch')
        finally:
            del os.environ['SOURCE_DATE_EPOCH']
        members, _ = self.members(manifest, self.work / 'epoch')
        self.assertEqual({member.mtime for member in members.values()}, {1000})

    def test_unsupported_entries_are_refused(self):
        (self.repository / 'link').symlink_to('CMakeLists.txt')
        git(self.repository, 'add', 'link')
        git(self.repository, 'commit', '-q', '-m', 'link')
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            PACKAGE.package(self.repository, self.work / 'link')
        git(self.repository, 'rm', '-q', 'link')
        (self.repository / 'SOURCE_REVISION').write_text('forged\n')
        git(self.repository, 'add', 'SOURCE_REVISION')
        git(self.repository, 'commit', '-q', '-m', 'reserved')
        with self.assertRaisesRegex(ValueError, 'reserved'):
            PACKAGE.package(self.repository, self.work / 'reserved')

    def test_missing_version_or_pin_is_refused(self):
        (self.repository / 'tests/CMakeLists.txt').write_text('CPMAddPackage(NAME SpecLab GIT_TAG main)\n')
        git(self.repository, 'commit', '-q', '-am', 'floating')
        with self.assertRaisesRegex(ValueError, 'pinned SpecLab'):
            PACKAGE.package(self.repository, self.work / 'floating')

    @unittest.skipUnless((ROOT / '.git').exists(), 'a Git checkout is required')
    def test_repository_manifest_lists_every_tracked_file(self):
        manifest = PACKAGE.package(ROOT, self.work / 'repository-archive')
        tracked = set(git(ROOT, 'ls-tree', '-r', '--name-only', 'HEAD').decode().splitlines())
        self.assertEqual({entry['path'] for entry in manifest['files']}, tracked | {'SOURCE_REVISION'})
        self.assertEqual(manifest['version'], PACKAGE.project_version((ROOT / 'CMakeLists.txt').read_text()))
        json.dumps(manifest)


if __name__ == '__main__':
    unittest.main()
