#!/usr/bin/env python3
"""Check the development file's structure and references, without accepting its content."""

import argparse
import datetime
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

SECTIONS = {
    'requirements': ('REQ', {'text', 'status', 'risks', 'controls', 'verifications', 'gaps'}),
    'risks': ('RISK', {'title', 'effect', 'controls', 'gaps', 'status', 'systemAssessment'}),
    'controls': ('CTRL', {'title', 'status', 'design', 'implementation'}),
    'verifications': ('VER', {'title', 'status', 'controls', 'test', 'scenario', 'evidence', 'limits'}),
    'gaps': ('GAP', {'title', 'status', 'issue', 'owner', 'closure'}),
    'dependencies': ('DEP', {'name', 'scope', 'version', 'provenance', 'license',
                              'classification', 'knownIssues', 'monitoring', 'gaps'}),
}
RELATIONS = {'risks': 'RISK', 'controls': 'CTRL', 'verifications': 'VER', 'gaps': 'GAP'}
STATES = {'requirements': {'implemented', 'planned'}, 'risks': {'open'},
          'controls': {'implemented'}, 'verifications': {'coverage-existing'},
          'gaps': {'open', 'closed'}}
IDENTIFIER = re.compile(r'\b(?:REQ|RISK|CTRL|VER|GAP|DEP)-\d{3}\b')
SHA = re.compile(r'[0-9a-f]{40}')
DOCUMENTS = ('Applicability.md', 'Plans.md', 'IEC_62304/SAD.md', 'IEC_62304/SDD.md',
             'IEC_62304/SOUP.md', 'ISO_14971/Risk_Management_File.md',
             'IEC_81001/Cybersecurity_SAD.md', 'ISO_13485/README.md',
             'IEC_62366/Usability_Engineering_File.md')


def check(root):
    """Return all structural errors; paths in JSON are relative to the repository root."""
    root = root.resolve()
    dossier = root / 'software_development_file'
    errors = []

    def fail(message):
        errors.append(message)

    def fields(value, expected, label):
        if not isinstance(value, dict) or set(value) != expected:
            fail(f'{label}: expected fields {sorted(expected)}')
            return False
        return True

    def local_file(value, label, base=root, allow_directory=False):
        if not isinstance(value, str) or not value.strip():
            fail(f'{label}: expected nonempty path')
            return
        parsed = urlsplit(value)
        if parsed.scheme or parsed.netloc or Path(parsed.path).is_absolute():
            fail(f'{label}: expected repository-relative file')
            return
        path = (base / unquote(parsed.path)).resolve()
        if not path.is_relative_to(root) or not (path.is_file() or (allow_directory and path.is_dir())):
            fail(f'{label}: missing or out-of-repository file {value}')

    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'duplicate JSON key {key}')
            result[key] = value
        return result

    try:
        data = json.loads((dossier / 'register.json').read_text(encoding='utf-8'),
                          object_pairs_hook=unique_pairs)
    except (OSError, ValueError) as error:
        return [f'register.json: {error}']
    if not fields(data, {'formatVersion', 'baseline', 'review', *SECTIONS}, 'register'):
        return errors
    if type(data['formatVersion']) is not int or data['formatVersion'] != 1:
        fail('register: unsupported formatVersion')
    baseline = data['baseline']
    if fields(baseline, {'version', 'commit', 'examinedCommit'}, 'baseline'):
        if not isinstance(baseline['version'], str) or not re.fullmatch(r'\d+\.\d+\.\d+', baseline['version']):
            fail('baseline: invalid version')
        for name in ('commit', 'examinedCommit'):
            if not isinstance(baseline[name], str) or not SHA.fullmatch(baseline[name]):
                fail(f'baseline: invalid {name}')

    entries = {}
    references = []
    for section, (prefix, expected) in SECTIONS.items():
        rows = data[section]
        if not isinstance(rows, list) or not rows:
            fail(f'{section}: expected nonempty list')
            continue
        for index, row in enumerate(rows):
            label = f'{section}[{index}]'
            if not fields(row, {'id', *expected}, label):
                continue
            identifier = row['id']
            if not isinstance(identifier, str) or not re.fullmatch(prefix + r'-\d{3}', identifier):
                fail(f'{label}: invalid id')
                continue
            if identifier in entries:
                fail(f'{label}: duplicate id {identifier}')
            entries[identifier] = row
            for key, value in row.items():
                if key in RELATIONS or key in {'design', 'implementation'}:
                    if not isinstance(value, list):
                        fail(f'{identifier}.{key}: expected list')
                        continue
                    if key in {'design', 'implementation'} and not value:
                        fail(f'{identifier}.{key}: expected nonempty list')
                    if any(not isinstance(item, str) or not item.strip() for item in value):
                        fail(f'{identifier}.{key}: expected nonempty strings')
                        continue
                    if len(set(value)) != len(value):
                        fail(f'{identifier}.{key}: duplicate reference')
                    for item in value:
                        if key in RELATIONS:
                            references.append((identifier, item, RELATIONS[key]))
                        else:
                            local_file(item, f'{identifier}.{key}')
                elif key == 'issue':
                    if type(value) is not int or value <= 0:
                        fail(f'{identifier}.issue: expected positive issue number')
                elif key == 'scenario' and value is None:
                    pass
                elif not isinstance(value, str) or not value.strip():
                    fail(f'{identifier}.{key}: expected nonempty string')
            if section in STATES and (not isinstance(row['status'], str) or row['status'] not in STATES[section]):
                fail(f'{identifier}: unsupported status')
            if section == 'dependencies':
                if (not isinstance(row['scope'], str) or row['scope'] not in {'deployed', 'test', 'build', 'verification'}):
                    fail(f'{identifier}: unsupported dependency scope')
                local_file(row['provenance'], f'{identifier}.provenance')
            if section == 'verifications':
                if not row['controls']:
                    fail(f'{identifier}: verification needs a control')
                local_file(row['test'], f'{identifier}.test')
                local_file(row['evidence'], f'{identifier}.evidence')
                if isinstance(row['test'], str) and isinstance(row['scenario'], str):
                    test = root / row['test']
                    if test.is_file() and f'"{row["scenario"]}"' not in test.read_text(encoding='utf-8'):
                        fail(f'{identifier}: scenario absent from test file')

    review = data['review']
    if fields(review, {'status', 'reviewer', 'reviewedCommit', 'date', 'decision', 'reservations'}, 'review'):
        if (not isinstance(review['status'], str) or review['status'] not in {'proposed', 'accepted', 'rejected'}):
            fail('review: unsupported status')
        if not isinstance(review['reviewer'], str) or not review['reviewer'].strip():
            fail('review: expected named reviewer')
        if not isinstance(review['reservations'], list):
            fail('review: reservations must be a list')
        else:
            for item in review['reservations']:
                references.append(('review', item, 'GAP'))
        if review['status'] == 'proposed':
            if any(review[name] is not None for name in ('reviewedCommit', 'date', 'decision')):
                fail('review: proposed review must not claim a decision')
        else:
            if not isinstance(review['reviewedCommit'], str) or not SHA.fullmatch(review['reviewedCommit']):
                fail('review: decision requires reviewed commit')
            try:
                datetime.date.fromisoformat(review['date'])
            except (TypeError, ValueError):
                fail('review: decision requires ISO date')
            if not isinstance(review['decision'], str) or not review['decision'].strip():
                fail('review: decision requires rationale')

    for source, target, prefix in references:
        if not isinstance(target, str) or not target.startswith(prefix + '-') or target not in entries:
            fail(f'{source}: unresolved {prefix} reference {target}')
    for row in data['requirements'] if isinstance(data['requirements'], list) else []:
        if not isinstance(row, dict) or not {'id', 'status', 'controls', 'verifications', 'gaps'} <= row.keys():
            continue
        if row['status'] == 'planned' and not row['gaps']:
            fail(f'{row["id"]}: planned requirement needs a tracked gap')
        if row['status'] == 'implemented' and (not row['controls'] or not row['verifications']):
            fail(f'{row["id"]}: implemented requirement needs control and verification')
        if isinstance(row['verifications'], list) and isinstance(row['controls'], list):
            for ref in row['verifications']:
                verification = entries.get(ref) if isinstance(ref, str) else None
                if verification and isinstance(verification.get('controls'), list) and not any(item in row['controls'] for item in verification['controls']):
                    fail(f'{row["id"]}: verification {ref} covers none of its controls')
        if isinstance(row['gaps'], list) and row['status'] == 'planned':
            if not any(isinstance(ref, str) and entries.get(ref, {}).get('status') == 'open' for ref in row['gaps']):
                fail(f'{row["id"]}: planned requirement needs an open gap')

    for subtree in ('regulatory', 'templates'):
        for name in DOCUMENTS:
            local_file(f'software_development_file/{subtree}/{name}', 'document inventory')
    local_file('software_development_file/README.md', 'document inventory')
    documents = sorted(dossier.rglob('*.md'))
    for document in documents:
        content = document.read_text(encoding='utf-8')
        if document.is_relative_to(dossier / 'regulatory') and re.search(r'\[à remplir\]', content, re.IGNORECASE):
            fail(f'{document.relative_to(root)}: untracked placeholder in filled document')
        # This dossier uses inline Markdown links; code blocks are excluded from link/ID checks.
        content = re.sub(r'```.*?```', '', content, flags=re.DOTALL)
        for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', content):
            parsed = urlsplit(target)
            if not parsed.scheme and not parsed.netloc:
                local_file(target, str(document.relative_to(root)), document.parent, allow_directory=True)
        for identifier in IDENTIFIER.findall(content):
            if identifier not in entries:
                fail(f'{document.relative_to(root)}: unresolved id {identifier}')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print('Development file: structure and references passed; content acceptance not assessed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
