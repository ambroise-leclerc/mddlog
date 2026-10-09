#!/usr/bin/env python3
"""Build a reproducible mddlog source archive and its version/dependency manifest (#121).

The archive holds every file Git tracks, under ``mddlog-<version>/``, plus ``SOURCE_REVISION``.
Entries are sorted by path; owner, group and permissions are normalized from the Git mode
(0644 or 0755); every timestamp is the commit time (or ``SOURCE_DATE_EPOCH``). The uncompressed
tar stream is therefore a function of the selected tree alone. The gzip layer is written with a
zero timestamp and no file name, but its bytes still depend on the zlib build, so the manifest
records the SHA-256 of both the tar stream and the compressed archive.

``--ref`` (default ``HEAD``) packages a commit: this is the release form. ``--worktree`` packages
the tracked files as they are on disk, for testing uncommitted changes; such an archive is marked
as such and is not a release artifact.
"""

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
MODES = {'100644': 0o644, '100755': 0o755}


def git(*arguments, root=ROOT, data=None):
    """Run git in the repository and return its raw standard output."""
    return subprocess.run(['git', '-C', str(root), *arguments], input=data, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def project_version(text):
    """Read the single version definition, project(mddlog VERSION X.Y.Z ...)."""
    match = re.search(r'project\(mddlog\s+VERSION\s+(\d+\.\d+\.\d+)\b', text)
    if not match:
        raise ValueError('CMakeLists.txt: project(mddlog VERSION X.Y.Z) not found')
    return match.group(1)


def tracked_entries(root, ref, worktree):
    """Return sorted (path, git mode, content) for every tracked regular file."""
    entries = []
    if worktree:
        listing = git('ls-files', '-s', '-z', root=root).split(b'\0')
        for record in filter(None, listing):
            header, path = record.decode('utf-8').split('\t', 1)
            mode = header.split()[0]
            if mode not in MODES:
                raise ValueError(f'{path}: unsupported tracked mode {mode} (links/submodules are not packaged)')
            file_path = root / path
            if not file_path.is_file() or file_path.is_symlink():
                raise ValueError(f'{path}: tracked file missing from the working tree')
            entries.append((path, mode, file_path.read_bytes()))
    else:
        listing = git('ls-tree', '-r', '-z', '--full-tree', ref, root=root).split(b'\0')
        objects = []
        for record in filter(None, listing):
            header, path = record.decode('utf-8').split('\t', 1)
            mode, kind, blob = header.split()
            if kind != 'blob' or mode not in MODES:
                raise ValueError(f'{path}: unsupported {kind} with mode {mode}')
            objects.append((path, mode, blob))
        if objects:
            output = git('cat-file', '--batch', root=root,
                         data=''.join(f'{blob}\n' for _, _, blob in objects).encode())
            cursor = 0
            for path, mode, blob in objects:
                newline = output.index(b'\n', cursor)
                name, kind, size = output[cursor:newline].decode().split()
                if name != blob or kind != 'blob':
                    raise ValueError(f'{path}: unexpected object {name} {kind}')
                start = newline + 1
                entries.append((path, mode, output[start:start + int(size)]))
                cursor = start + int(size) + 1
    for path, _, _ in entries:
        if path.startswith('/') or '..' in Path(path).parts or path == 'SOURCE_REVISION':
            raise ValueError(f'{path}: unsafe or reserved archive path')
    return sorted(entries, key=lambda entry: entry[0].encode('utf-8'))


def build_tar(prefix, entries, mtime):
    """Serialize entries deterministically as a GNU tar stream."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.GNU_FORMAT) as archive:
        for path, mode, content in entries:
            info = tarfile.TarInfo(f'{prefix}/{path}')
            info.size = len(content)
            info.mode = MODES[mode]
            info.mtime = mtime
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            archive.addfile(info, io.BytesIO(content))
    return buffer.getvalue()


def dependencies(tests_text):
    """Pinned build/test dependencies read from the CMake files that pin them."""
    speclab = re.search(r'GITHUB_REPOSITORY\s+ambroise-leclerc/SpecLab\s+#\s*(SpecLab\s+\S+)\s+GIT_TAG\s+([0-9a-f]{40})',
                        tests_text)
    if not speclab:
        raise ValueError('tests/CMakeLists.txt: pinned SpecLab revision not found')
    return [
        {'name': 'C++ standard library and its std module', 'scope': 'deployed',
         'pin': 'toolchain of the build (see mddlog-build-info.json and docs/compatibility-matrix.md)'},
        {'name': 'Threads', 'scope': 'deployed', 'pin': 'toolchain of the build'},
        {'name': 'CMake', 'scope': 'build', 'pin': '4.0-4.3 (CMakeLists.txt rejects 4.4+)'},
        {'name': 'Ninja', 'scope': 'build', 'pin': 'module-capable Ninja (CMakeLists.txt rejects other generators)'},
        {'name': 'CPM.cmake', 'scope': 'build', 'pin': 'vendored copy cmake/CPM.cmake'},
        {'name': 'SpecLab', 'scope': 'test', 'pin': f'{speclab.group(1)} {speclab.group(2)}',
         'note': 'fetched only when MDDLOG_BUILD_TESTS=ON; never part of the installed package'},
    ]


def package(root, output, ref='HEAD', worktree=False):
    """Write the archive and manifest; return the manifest dictionary."""
    root = Path(root).resolve()
    commit = git('rev-parse', '--verify', f'{ref}^{{commit}}', root=root).decode().strip()
    if os.environ.get('SOURCE_DATE_EPOCH'):
        mtime = int(os.environ['SOURCE_DATE_EPOCH'])
    else:
        mtime = int(git('show', '-s', '--format=%ct', commit, root=root).decode().strip())
    entries = tracked_entries(root, commit, worktree)
    contents = {path: content for path, _, content in entries}
    version = project_version(contents['CMakeLists.txt'].decode('utf-8'))
    state = 'worktree' if worktree else 'commit'
    entries.append(('SOURCE_REVISION', '100644', f'{commit}\n{state}\n'.encode()))
    entries.sort(key=lambda entry: entry[0].encode('utf-8'))
    prefix = f'mddlog-{version}'
    tar_bytes = build_tar(prefix, entries, mtime)
    compressed = io.BytesIO()
    with gzip.GzipFile(filename='', mode='wb', fileobj=compressed, mtime=0, compresslevel=9) as stream:
        stream.write(tar_bytes)
    archive_bytes = compressed.getvalue()

    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    archive_name = f'{prefix}-src.tar.gz'
    (output / archive_name).write_bytes(archive_bytes)
    manifest = {
        'formatVersion': 1,
        'package': 'mddlog',
        'version': version,
        'sourceRevision': {'commit': commit, 'state': state},
        'sourceDateEpoch': mtime,
        'archive': {'name': archive_name, 'prefix': prefix,
                    'tarSha256': hashlib.sha256(tar_bytes).hexdigest(),
                    'sha256': hashlib.sha256(archive_bytes).hexdigest(),
                    'gzip': 'mtime 0, no file name, level 9; bytes depend on zlib'},
        'files': [{'path': path, 'mode': f'{MODES[mode]:04o}', 'size': len(content),
                   'sha256': hashlib.sha256(content).hexdigest()} for path, mode, content in entries],
        'dependencies': dependencies(contents['tests/CMakeLists.txt'].decode('utf-8')),
    }
    (output / f'{prefix}-src.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / f'{archive_name}.sha256').write_text(f'{manifest["archive"]["sha256"]}  {archive_name}\n', encoding='utf-8')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--ref', default='HEAD', help='commit to package (default HEAD)')
    source.add_argument('--worktree', action='store_true', help='package tracked files from the working tree')
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = package(args.root, args.output, args.ref, args.worktree)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'package-source: {error}', file=sys.stderr)
        return 2
    print(f'{manifest["archive"]["name"]}: tar {manifest["archive"]["tarSha256"]}, '
          f'archive {manifest["archive"]["sha256"]}, {len(manifest["files"])} files, '
          f'{manifest["sourceRevision"]["state"]} {manifest["sourceRevision"]["commit"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
