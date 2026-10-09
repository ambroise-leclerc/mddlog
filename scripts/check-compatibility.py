#!/usr/bin/env python3
"""Check the public-surface inventory and compatibility evidence of mddlog (#121, ADR-007).

Without compiling C++, this verifies that docs/api/public-surface.json matches the repository:
registered module files and their targets/components, the umbrella's exported names, the
installed targets and package components, and the version constant of every persistent or
exchanged format. With ``--git`` it also compares the inventory with the published release tags:
the recorded ``since`` versions, the absence of unrecorded removals, and an archive fixture for
every audit-producing release. Finally, every migration-guide snippet must equal the compiled
region it quotes. It does not prove source compatibility by itself: compiled consumers do.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

TIERS = {'stable-candidate', 'extension', 'format-codec', 'implementation-module',
         'reduction-candidate', 'test-double', 'link-only'}
COMPONENTS = {'core', 'full', 'file_storage', 'audit_tool'}
VERSION = re.compile(r'\d+\.\d+\.\d+')
RELEASE_TAG = re.compile(r'v(\d+)\.(\d+)\.(\d+)')
SNIPPET = re.compile(r'<!-- snippet: (?P<path>[^#\s]+)#(?P<name>[a-z0-9-]+) -->\n```cpp\n(?P<body>.*?)\n```', re.DOTALL)
# Releases from which the audit persistence formats exist (ADR-004 shipped in 0.2.0).
FIRST_AUDIT_RELEASE = (0, 2, 0)


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key {key}')
        result[key] = value
    return result


def module_name(source):
    match = re.search(r'^export module ([a-z0-9.]+);', source, re.MULTILINE)
    return match.group(1) if match else None


def umbrella_names(source):
    """Names the umbrella exports: using-declarations and the functions it defines."""
    names = set(re.findall(r'^using [a-z]+::([A-Za-z0-9_]+);', source, re.MULTILINE))
    names |= set(re.findall(r'^constexpr [^\n(]*?\b([A-Za-z0-9_]+)\(\)', source, re.MULTILINE))
    return names


def cmake_block(text, opening, closing=')'):
    start = text.find(opening)
    if start < 0:
        return ''
    return text[start:text.find(closing, start + len(opening))]


def snippet_regions(source):
    regions = {}
    for match in re.finditer(r'^// \[migration:(?P<name>[a-z0-9-]+)\]\n(?P<body>.*?)^// \[/migration:(?P=name)\]$',
                             source, re.MULTILINE | re.DOTALL):
        regions[match.group('name')] = match.group('body').rstrip('\n')
    return regions


def git_lines(root, *arguments):
    result = subprocess.run(['git', '-C', str(root), *arguments], capture_output=True, text=True)
    # git grep exits with 1 when nothing matches: an empty listing, not a failure.
    if result.returncode == 1 and arguments[0] == 'grep' and not result.stderr.strip():
        return ''
    if result.returncode:
        raise RuntimeError(f'git {" ".join(arguments)}: {result.stderr.strip()}')
    return result.stdout


def released_tags(root):
    tags = []
    for line in git_lines(root, 'tag', '--list', 'v*').splitlines():
        match = RELEASE_TAG.fullmatch(line.strip())
        if match:
            tags.append((tuple(int(part) for part in match.groups()), line.strip()))
    return sorted(tags)


def check(root, use_git=False):
    """Return a list of errors; an empty list means every checked relation holds."""
    root = root.resolve()
    errors = []
    fail = errors.append
    try:
        surface = json.loads((root / 'docs/api/public-surface.json').read_text(encoding='utf-8'),
                             object_pairs_hook=unique_pairs)
    except (OSError, ValueError) as error:
        return [f'public-surface.json: {error}']
    expected = {'formatVersion', 'status', 'policy', 'tiers', 'targets', 'components', 'formats',
                'modules', 'umbrella', 'removed'}
    if not isinstance(surface, dict) or set(surface) != expected:
        return [f'public-surface.json: expected fields {sorted(expected)}']
    if surface['formatVersion'] != 1 or surface['status'] not in {'candidate', 'frozen'}:
        fail('public-surface.json: unsupported formatVersion or status')
    if set(surface['tiers']) != TIERS:
        fail('public-surface.json: tier definitions differ from the checked set')
    if not (root / surface['policy']).is_file():
        fail(f'public-surface.json: missing policy {surface["policy"]}')

    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    config = (root / 'cmake/mddlogConfig.cmake.in').read_text(encoding='utf-8')

    # Modules: every registered interface unit is inventoried with its real target/component.
    registered = re.findall(r'(include/mddlog/[A-Za-z0-9_/]+\.cppm)', cmake)
    core_files = set(re.findall(r'(include/mddlog/[A-Za-z0-9_/]+\.cppm)', cmake_block(cmake, 'set(MDDLOG_CORE_MODULE_FILES')))
    storage_files = set(re.findall(r'(include/mddlog/[A-Za-z0-9_/]+\.cppm)',
                                   cmake_block(cmake, 'if(MDDLOG_BUILD_FILE_STORAGE)', '\nendif()')))
    actual = {}
    for path in registered:
        file = root / path
        name = module_name(file.read_text(encoding='utf-8')) if file.is_file() else None
        if not name:
            fail(f'CMakeLists.txt: {path} is missing or declares no module')
            continue
        component = 'core' if path in core_files else ('file_storage' if path in storage_files else 'full')
        actual[name] = {'file': path, 'target': 'mddlog::core' if component == 'core' else 'mddlog::mddlog',
                        'component': component}
    for path in sorted({str(p.relative_to(root)) for p in (root / 'include/mddlog').rglob('*.cppm')} - set(registered)):
        fail(f'{path}: module interface not registered in a FILE_SET')
    modules = surface['modules']
    for name in sorted(set(actual) - set(modules)):
        fail(f'module {name}: not inventoried')
    for name in sorted(set(modules) - set(actual)):
        fail(f'module {name}: inventoried but not registered')
    for name in sorted(set(actual) & set(modules)):
        entry = modules[name]
        if not isinstance(entry, dict) or set(entry) != {'file', 'target', 'component', 'tier', 'since'}:
            fail(f'module {name}: expected file, target, component, tier and since')
            continue
        for field in ('file', 'target', 'component'):
            if entry[field] != actual[name][field]:
                fail(f'module {name}: {field} is {entry[field]}, CMake says {actual[name][field]}')
        if entry['tier'] not in TIERS - {'reduction-candidate', 'test-double', 'link-only'}:
            fail(f'module {name}: unsupported tier {entry["tier"]}')
        if entry['component'] == 'core' and entry['tier'] != 'stable-candidate':
            fail(f'module {name}: a governed core module is imported directly and must be stable-candidate')
        if entry['since'] != 'unreleased' and not VERSION.fullmatch(str(entry['since'])):
            fail(f'module {name}: invalid since {entry["since"]}')

    # Umbrella names.
    umbrella = surface['umbrella']
    umbrella_file = root / umbrella.get('file', '')
    exported = umbrella_names(umbrella_file.read_text(encoding='utf-8')) if umbrella_file.is_file() else set()
    if not exported:
        fail('umbrella: no exported names found')
    names = umbrella.get('names', {})
    for name in sorted(exported - set(names)):
        fail(f'umbrella name {name}: exported but not inventoried')
    for name in sorted(set(names) - exported):
        fail(f'umbrella name {name}: inventoried but not exported')
    for name, entry in sorted(names.items()):
        if not isinstance(entry, dict) or set(entry) != {'tier', 'since'}:
            fail(f'umbrella name {name}: expected tier and since')
            continue
        if entry['tier'] not in {'stable-candidate', 'extension', 'reduction-candidate', 'test-double'}:
            fail(f'umbrella name {name}: unsupported tier {entry["tier"]}')
        if entry['since'] != 'unreleased' and not VERSION.fullmatch(str(entry['since'])):
            fail(f'umbrella name {name}: invalid since {entry["since"]}')
    removed = surface['removed']
    if not isinstance(removed, list):
        fail('removed: expected a list')
        removed = []
    for index, entry in enumerate(removed):
        if not isinstance(entry, dict) or set(entry) != {'name', 'kind', 'deprecatedIn', 'removedIn', 'migration'}:
            fail(f'removed[{index}]: expected name, kind, deprecatedIn, removedIn and migration')
        elif not (root / entry['migration'].split('#')[0]).is_file():
            fail(f'removed[{index}]: missing migration guide {entry["migration"]}')

    # Targets and components, as installed and as the package configuration declares them.
    installed = cmake_block(cmake, 'set(_mddlog_installed_targets')
    for target, cmake_name in (('mddlog::mddlog', 'mddlog'), ('mddlog::core', 'mddlog-core'),
                               ('mddlog::mddlog_options', 'mddlog_options')):
        if not re.search(rf'(?<![\w-]){re.escape(cmake_name)}(?![\w-])', installed):
            fail(f'target {target}: not installed')
    if 'mddlog_warnings' in installed or re.search(r'install\(TARGETS[^)]*mddlog_warnings', cmake):
        fail('mddlog_warnings must stay build-only')
    if 'EXPORT_NAME audit_tool' not in cmake or 'list(APPEND _mddlog_installed_targets mddlog_audit)' not in cmake:
        fail('target mddlog::audit_tool: not exported under that name')
    if set(surface['targets']) != {'mddlog::core', 'mddlog::mddlog', 'mddlog::audit_tool', 'mddlog::mddlog_options'}:
        fail('targets: inventory differs from the installed export set')
    if set(surface['components']) != COMPONENTS:
        fail('components: inventory differs from the checked set')
    declared = set(re.findall(r'\^\(([a-z_|]+)\)\$', config))
    if {'|'.join(sorted(COMPONENTS, key=['core', 'full', 'file_storage', 'audit_tool'].index))} != declared:
        fail('mddlogConfig.cmake.in: component list differs from the inventory')
    for component in COMPONENTS:
        if f'mddlog_{component}_FOUND' not in config:
            fail(f'mddlogConfig.cmake.in: component {component} never reports FOUND')

    # Format versions: a changed constant without an inventory change is a silent format change.
    for format_id, entry in sorted(surface['formats'].items()):
        path = root / entry.get('file', '')
        if not path.is_file():
            fail(f'format {format_id}: missing file {entry.get("file")}')
            continue
        found = re.findall(entry['pattern'], path.read_text(encoding='utf-8'))
        if len(found) != 1 or int(found[0]) != entry['version']:
            fail(f'format {format_id}: source declares {found}, inventory says version {entry["version"]}')

    # Migration snippets equal the compiled regions they quote.
    guide = root / 'docs/migration/v0.2-to-1.0.md'
    if not guide.is_file():
        fail('docs/migration/v0.2-to-1.0.md: missing migration guide')
    else:
        quoted = list(SNIPPET.finditer(guide.read_text(encoding='utf-8')))
        if not quoted:
            fail('migration guide: no compiled snippet')
        for match in quoted:
            source = root / match.group('path')
            regions = snippet_regions(source.read_text(encoding='utf-8')) if source.is_file() else {}
            if match.group('name') not in regions:
                fail(f'migration guide: region {match.group("path")}#{match.group("name")} not found')
            elif regions[match.group('name')] != match.group('body'):
                fail(f'migration guide: snippet {match.group("name")} differs from {match.group("path")}')

    if use_git:
        errors.extend(check_history(root, surface))
    return errors


def prepared_version(root):
    """Version declared by project(mddlog VERSION ...), or None."""
    try:
        text = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    except OSError:
        return None
    match = re.search(r'project\(\s*mddlog\s+VERSION\s+(\d+)\.(\d+)\.(\d+)\b', text)
    return tuple(int(part) for part in match.groups()) if match else None


def check_history(root, surface):
    """Compare the inventory with the published release tags."""
    errors = []
    fail = errors.append
    try:
        tags = released_tags(root)
    except RuntimeError as error:
        return [str(error)]
    if not tags:
        return ['--git: no release tag available (fetch tags or omit --git)']
    removed = {entry['name'] for entry in surface['removed'] if isinstance(entry, dict)}
    # The version being prepared: project(VERSION) once bumped by the release procedure, before its
    # tag exists. New entries may already name it; every published version is still checked.
    accepted = {'unreleased'}
    declared = prepared_version(root)
    if declared is None:
        fail('CMakeLists.txt: no project(mddlog VERSION X.Y.Z)')
    elif declared > max(version for version, _ in tags):
        accepted.add('.'.join(map(str, declared)))
    first_names, first_modules = {}, {}
    for version, tag in tags:
        try:
            text = git_lines(root, 'show', f'{tag}:include/mddlog/mddlog.cppm')
            listing = git_lines(root, 'grep', '-h', '-E', '^export module ', tag, '--', 'include')
        except RuntimeError as error:
            fail(f'release {tag}: {error}')
            continue
        for name in umbrella_names(text):
            first_names.setdefault(name, version)
        for line in listing.splitlines():
            name = module_name(line.split(':', 1)[-1])
            if name:
                first_modules.setdefault(name, version)
        if version >= FIRST_AUDIT_RELEASE:
            fixture = root / f'tests/archives/audit-export/{tag}.json'
            if not fixture.is_file():
                fail(f'release {tag}: no audit archive fixture tests/archives/audit-export/{tag}.json')
    for kind, released, current in (('umbrella name', first_names, surface['umbrella']['names']),
                                    ('module', first_modules, surface['modules'])):
        for name, version in sorted(released.items()):
            since = '.'.join(map(str, version))
            if name not in current:
                if name not in removed:
                    fail(f'{kind} {name}: released in {since}, now absent without a removed[] record')
            elif current[name]['since'] != since:
                fail(f'{kind} {name}: since {current[name]["since"]}, first released in {since}')
        for name, entry in sorted(current.items()):
            if name not in released and entry['since'] not in accepted:
                fail(f'{kind} {name}: since {entry["since"]}, but absent from every release tag '
                     f'and not the version in preparation')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--git', action='store_true', help='also compare with the release tags')
    args = parser.parse_args()
    errors = check(args.root, args.git)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    scope = 'inventory, formats, snippets and release history' if args.git else 'inventory, formats and snippets'
    print(f'Compatibility: {scope} consistent; compatibility itself is shown by compiled consumers.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
