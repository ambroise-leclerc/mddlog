#!/usr/bin/env python3
"""Reproduce #130's source readability counts after clang-format, without claiming timing gains."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
USAGE = ROOT / 'examples/ContextualUsage.cpp'
WORKER = ROOT / 'examples/InventoryWorker.hpp'


def body(path, name):
    source = path.read_text(encoding='utf-8')
    match = re.search(r'^[ \t]*(?:\[\[nodiscard\]\][ \t]*)?(?:[\w:<>&*]+[ \t]+)+'
                      + re.escape(name) + r'\([^{};]*?\)\s*(?:noexcept\s*)?\{', source, re.MULTILINE)
    if not match:
        raise ValueError(f'missing function definition: {path}:{name}')
    start = match.end()
    depth = 1
    for end in range(start, len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if depth == 0:
                return source[start:end]
    raise ValueError(f'unterminated body: {path}:{name}')


def counts(path, names):
    source = '\n'.join(body(path, name) for name in names)
    # Include policy/control flow and helper bodies, exclude empty lines and comments.
    lines = sum(bool(line.strip()) and not line.lstrip().startswith('//') for line in source.splitlines())
    projections = len(re.findall(r'\.(?:component|operationId|correlationId|category|action|actor|target|requirementRef|riskRef)\b', source))
    projections += source.count('[pump:')  # Repeated textual projections in TextLogger calls.
    return lines, projections


def main():
    print('| Usage | Lignes avant | Lignes après | Projections invariantes avant/après |')
    print('| --- | ---: | ---: | ---: |')
    pairs = (
        ('Composant', USAGE, ('componentBefore',), ('componentAfter',)),
        ('Opération', USAGE, ('operationBefore',), ('operationAfter',)),
        ('Producteur gouverné', USAGE, ('governedBefore',), ('governedAfter',)),
        ('Action auditée (helper inclus)', USAGE, ('auditBefore', 'auditWriteBefore'), ('auditAfter',)),
        ('Plusieurs producteurs', USAGE, ('producersBefore',), ('producersAfter',)),
        ('Composant stock concret', WORKER, ('adjustBefore',), ('apply',)),
    )
    for title, path, before, after in pairs:
        before_lines, before_fields = counts(path, before)
        after_lines, after_fields = counts(path, after)
        print(f'| {title} | {before_lines} | {after_lines} | {before_fields}/{after_fields} |')


if __name__ == '__main__':
    main()
