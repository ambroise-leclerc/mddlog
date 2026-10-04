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


def cpp_scenarios(source):
    """Recognize literal IDs passed to speclab::Test, excluding comments and other literals.

    This lexical check does not evaluate macros, inactive preprocessor branches or C++ semantics.
    """
    token_pattern = re.compile(
        r'//[^\n]*|/\*.*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
        r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|::|[^\s]',
        re.DOTALL,
    )
    tokens = [match.group() for match in token_pattern.finditer(source)
              if not match.group().startswith(('//', '/*'))]
    ids = set()
    for index in range(len(tokens) - 3):
        if tokens[index:index + 3] != ['speclab', '::', 'Test']:
            continue
        cursor = index + 3
        if tokens[cursor] == '<':
            depth = 1
            cursor += 1
            while cursor < len(tokens) and depth:
                depth += (tokens[cursor] == '<') - (tokens[cursor] == '>')
                cursor += 1
            if depth:
                continue
        if cursor + 2 < len(tokens) and tokens[cursor] == '(' and tokens[cursor + 2] in {',', ')'}:
            literal = tokens[cursor + 1]
            if re.fullmatch(r'"[A-Za-z0-9_-]+"', literal):
                ids.add(literal[1:-1])
    return ids


def markdown_text(source):
    """Remove fenced blocks and inline code from the documented Markdown subset."""
    source = re.sub(r'^ {0,3}(`{3,}|~{3,})[^\n]*\n.*?^ {0,3}\1[^\n]*$', '',
                    source, flags=re.MULTILINE | re.DOTALL)
    return re.sub(r'(`+)(?!`).*?(?<!`)\1(?!`)', '', source, flags=re.DOTALL)


def markdown_links(source):
    """Read inline links with balanced destinations, angle brackets and optional titles.

    Reference-style links are outside this dossier's format and rejected by the caller.
    """
    for match in re.finditer(r'\[[^\]\n]*\]\(', source):
        cursor = match.end()
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1
        target = []
        angle = cursor < len(source) and source[cursor] == '<'
        if angle:
            cursor += 1
        depth = 0
        while cursor < len(source):
            char = source[cursor]
            if char == '\\' and cursor + 1 < len(source):
                target.append(source[cursor + 1])
                cursor += 2
                continue
            if angle and char == '>':
                cursor += 1
                break
            if not angle:
                if char == '(':
                    depth += 1
                elif char == ')':
                    if not depth:
                        break
                    depth -= 1
                elif char.isspace() and not depth:
                    break
            target.append(char)
            cursor += 1
        else:
            raise ValueError('unterminated inline link')
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1
        if cursor < len(source) and source[cursor] in {'"', "'", '('}:
            delimiter = ')' if source[cursor] == '(' else source[cursor]
            cursor += 1
            while cursor < len(source) and source[cursor] != delimiter:
                cursor += 2 if source[cursor] == '\\' else 1
            if cursor >= len(source):
                raise ValueError('unterminated link title')
            cursor += 1
            while cursor < len(source) and source[cursor].isspace():
                cursor += 1
        if cursor >= len(source) or source[cursor] != ')':
            raise ValueError('malformed inline link')
        yield ''.join(target)


def markdown_anchors(source):
    """GitHub-style heading slugs for plain/inline-formatted ATX and setext headings."""
    source = re.sub(r'^ {0,3}(`{3,}|~{3,})[^\n]*\n.*?^ {0,3}\1[^\n]*$', '',
                    source, flags=re.MULTILINE | re.DOTALL)
    headings = []
    lines = source.splitlines()
    for index, line in enumerate(lines):
        heading = re.match(r'^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$', line)
        if heading:
            headings.append(heading.group(1))
        elif index and re.fullmatch(r' {0,3}(?:=+|-+)\s*', line) and lines[index - 1].strip():
            headings.append(lines[index - 1].strip())
    anchors = set()
    for heading in headings:
        heading = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', heading)
        slug = re.sub(r'[^\w\s-]', '', heading.lower()).replace(' ', '-').replace('\t', '-')
        unique = slug
        suffix = 0
        while unique in anchors:
            suffix += 1
            unique = f'{slug}-{suffix}'
        anchors.add(unique)
    return anchors


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
        path = (base if not parsed.path else base / unquote(parsed.path)).resolve()
        if not path.is_relative_to(root) or not (path.is_file() or (allow_directory and path.is_dir())):
            fail(f'{label}: missing or out-of-repository file {value}')
            return
        if parsed.fragment:
            if path.suffix.lower() != '.md' or unquote(parsed.fragment) not in markdown_anchors(path.read_text(encoding='utf-8')):
                fail(f'{label}: missing Markdown anchor {value}')

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
    if type(data['formatVersion']) is not int or data['formatVersion'] != 2:
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
                    if test.is_file() and row['scenario'] not in cpp_scenarios(test.read_text(encoding='utf-8')):
                        fail(f'{identifier}: scenario absent from test file')

    review = data['review']
    if fields(review, {'status', 'reviewer', 'reviewedCommit', 'date', 'decision', 'reservations', 'acceptedGaps'}, 'review'):
        if (not isinstance(review['status'], str) or review['status'] not in {'proposed', 'accepted', 'rejected'}):
            fail('review: unsupported status')
        if not isinstance(review['reviewer'], str) or not review['reviewer'].strip():
            fail('review: expected named reviewer')
        if not isinstance(review['reservations'], list):
            fail('review: reservations must be a list')
        else:
            for item in review['reservations']:
                references.append(('review', item, 'GAP'))
        if not isinstance(review['acceptedGaps'], dict):
            fail('review: acceptedGaps must map gap IDs to explicit rationales')
        else:
            for identifier, rationale in review['acceptedGaps'].items():
                references.append(('review.acceptedGaps', identifier, 'GAP'))
                if not isinstance(rationale, str) or not rationale.strip():
                    fail(f'review.acceptedGaps: {identifier} needs an explicit rationale')
            if review['status'] != 'accepted' and review['acceptedGaps']:
                fail('review: acceptedGaps requires an accepted review')
        if review['status'] == 'accepted' and isinstance(review['reservations'], list):
            for ref in review['reservations']:
                if isinstance(ref, str) and entries.get(ref, {}).get('status') != 'closed':
                    fail(f'review: reservation {ref} must be closed before acceptance')
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
        if row['status'] == 'implemented':
            for field, expected_status in (('controls', 'implemented'), ('verifications', 'coverage-existing')):
                if isinstance(row[field], list):
                    for ref in row[field]:
                        if isinstance(ref, str) and ref in entries and entries[ref].get('status') != expected_status:
                            fail(f'{row["id"]}: incompatible {field} status for {ref}')
            if isinstance(review, dict) and review.get('status') == 'accepted' and isinstance(row['gaps'], list):
                accepted_gaps = review.get('acceptedGaps', {})
                for ref in row['gaps']:
                    if isinstance(ref, str) and entries.get(ref, {}).get('status') == 'open':
                        if not isinstance(accepted_gaps, dict) or not accepted_gaps.get(ref):
                            fail(f'{row["id"]}: open gap {ref} needs explicit acceptance rationale')
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
        identifier_content = re.sub(r'```.*?```|~~~.*?~~~', '', content, flags=re.DOTALL)
        content = markdown_text(content)
        if re.search(r'\[[^\]]+\]\[[^\]]*\]|^ {0,3}\[[^\]]+\]:', content, re.MULTILINE):
            fail(f'{document.relative_to(root)}: reference-style links are unsupported; use inline links')
        try:
            for target in markdown_links(content):
                parsed = urlsplit(target)
                if not parsed.scheme and not parsed.netloc:
                    base = document if not parsed.path else document.parent
                    local_file(target, str(document.relative_to(root)), base, allow_directory=True)
        except ValueError as error:
            fail(f'{document.relative_to(root)}: {error}')
        for identifier in IDENTIFIER.findall(identifier_content):
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
