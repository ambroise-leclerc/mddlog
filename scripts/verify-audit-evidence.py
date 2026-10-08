#!/usr/bin/env python3
"""Check the #120 evidence from 2026-10-08 offline, including hashes and the real-chain oracle."""
import argparse
import gzip
import hashlib
import io
from importlib.util import module_from_spec, spec_from_file_location
import json
from pathlib import Path, PurePosixPath
import tempfile
import zipfile

DEFAULT = Path(__file__).resolve().parents[1] / 'docs/validation/audit-robustness/2026-10-08'
# Original GitHub digest, also published in the README; independent of a supplied manifest.
ORIGINAL_SHA256 = '9a98a554725bd4d9d8b6d5cc31fa23a86402fd2855fbbe7531ad33fa8ced2855'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def safe_name(name):
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and '..' not in path.parts
            and str(path) == name and '\\' not in name, 'unsafe evidence member')
    return name


def checked_file(root, entry):
    path = root / safe_name(entry['file'])
    require(path.resolve().is_relative_to(root.resolve()), 'evidence file escapes its root')
    require(path.stat().st_size <= 16 * 1024 * 1024, 'evidence file exceeds input budget')
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest() == entry['sha256'], 'evidence file SHA-256 mismatch')
    return data


def verify_log(root, entry, maximum=16 * 1024 * 1024):
    require(maximum > 0, 'invalid workflow log budget')
    with gzip.GzipFile(fileobj=io.BytesIO(checked_file(root, entry))) as source:
        length, digest = bounded_digest(source, maximum, 'workflow log')
    require(length > 0 and digest == entry['uncompressed_sha256'], 'workflow log content mismatch')


def bounded_digest(source, maximum, label):
    """Count actual output bytes, independently of compressed-file metadata."""
    require(maximum >= 0, 'invalid decompression budget')
    digest = hashlib.sha256()
    length = 0
    while block := source.read(min(65536, maximum - length + 1)):
        length += len(block)
        require(length <= maximum, f'{label} exceeds decompression budget')
        digest.update(block)
    return length, digest.hexdigest()


def verify_evidence(root):
    manifest_file = root / 'manifest.json'
    require(manifest_file.stat().st_size <= 65536, 'manifest exceeds input budget')
    manifest = json.loads(manifest_file.read_text())
    require(manifest['schema'] == 1 and manifest['issue'] == 120, 'unsupported evidence manifest')
    require(manifest['tested_revision'] == manifest['run']['head_sha']
            and manifest['run']['event'] == 'workflow_dispatch'
            and manifest['run']['conclusion'] == 'success', 'campaign provenance mismatch')
    descriptor = manifest['archive']
    require(manifest['artifact']['digest'] == 'sha256:' + descriptor['sha256'], 'original artifact digest mismatch')
    data = checked_file(root, descriptor)
    require(hashlib.sha256(data).hexdigest() == ORIGINAL_SHA256, 'original archive reference mismatch')
    require(len(data) == descriptor['bytes'] and len(data) <= 16 * 1024 * 1024, 'archive size mismatch')
    for log in manifest['logs']:
        require(log['revision'] == manifest['tested_revision'] and log['conclusion'] == 'success',
                'workflow log provenance mismatch')
        verify_log(root, log)
    inventory = {}
    for line in checked_file(root, manifest['inventory']).decode().splitlines():
        digest, separator, name = line.partition('  ')
        require(separator and len(digest) == 64 and all(char in '0123456789abcdef' for char in digest),
                'invalid evidence inventory')
        safe_name(name)
        require(name not in inventory, 'duplicate inventory member')
        inventory[name] = digest
    # Never extract arbitrary ZIP paths. Only the checked traces enter a private temporary directory.
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        members = archive.infolist()
        names = [safe_name(member.filename) for member in members]
        require(len(names) == len(set(names)) == len(inventory) == descriptor['entries']
                and set(names) == set(inventory), 'archive inventory mismatch')
        total = sum(member.file_size for member in members)
        require(total == descriptor['uncompressed_bytes'] and total <= 200 * 1024 * 1024
                and all(member.file_size <= 16 * 1024 * 1024 for member in members), 'archive exceeds evidence budget')
        observed = 0
        for member in members:
            budget = min(16 * 1024 * 1024, 200 * 1024 * 1024 - observed)
            with archive.open(member) as source:
                length, digest = bounded_digest(source, budget, 'archive member')
            observed += length
            require(length == member.file_size, 'archive member size mismatch')
            require(digest == inventory[member.filename],
                    'archive member SHA-256 mismatch')
        require(observed == total, 'archive decompressed total mismatch')
        reports = {}
        require(set(manifest['reports']) == {'chain', 'readers', 'storage', 'volume'}, 'missing campaign report')
        for kind, entry in manifest['reports'].items():
            report_bytes = checked_file(root, entry)
            require(report_bytes == archive.read(safe_name(entry['archive_member'])), 'report copy differs from original')
            reports[kind] = json.loads(report_bytes)
            require(reports[kind]['status'] == entry['status'], 'report status mismatch')
        chain, readers, storage, volume = (reports[key] for key in ('chain', 'readers', 'storage', 'volume'))
        require(chain['status'] == readers['status'] == volume['status'] == 'PASS', 'required campaign did not pass')
        require(chain['revision'] == readers['revision'] == manifest['tested_revision'], 'report revision mismatch')
        profile = chain['profile']
        require((profile['seed'], profile['seeds'], profile['boots'], profile['records'], profile['outage'])
                == (120, 32, 24, 64, 1000), 'prolonged profile mismatch')
        require(chain['sanitizer_options']['ASAN_OPTIONS'] == 'detect_leaks=1'
                and readers['leak_detection'], 'leak detection not enabled')
        deployment, tools = chain['deployment'], chain['tools']
        require(deployment['status'] == tools['status'] == 'PASS'
                and deployment['repetitions'] == len(deployment['commands']) == 32
                and len(tools['commands']) == 2, 'required integrations missing')
        for index, command in enumerate(deployment['commands']):
            require(command[:2] == ['sudo', '--non-interactive'] and '--require-isolation' in command,
                    'deployment isolation not required')
            member = str(PurePosixPath(manifest['reports']['chain']['archive_member']).parent / f'deployment-{index}.log')
            log = archive.read(member).decode()
            require('PASS' in log and 'SKIP' not in log, 'deployment did not pass')
        require(set(tools['corpus_sha256']) == {'v0.2.0.json', 'v0.3.0.json'}, 'release archive replay missing')
        require(readers['fuzz']['inputs'] == 1000000 and readers['fuzz']['status'] == 'PASS'
                and readers['sha256']['inputs'] == 268 and readers['sha256']['status'] == 'PASS'
                and readers['coverage']['status'] == 'COLLECTED'
                and {'AuditCanonical.cppm', 'AuditLayout.cppm', 'AuditLedger.cppm', 'AuditLog.cppm',
                     'AuditLogVerifier.cppm', 'AuditStore.cppm'} <= set(readers['coverage']['units']),
                'reader campaign incomplete')
        require(storage['status'] == 'PARTIAL' and storage['iterations'] == 100
                and storage['summary'] == {'PASS': 2609, 'FAIL': 0, 'SKIP': 1}, 'storage campaign accounting mismatch')
        require(sum(case['status'] == 'PASS' for case in storage['cases']) == 2609
                and [case['name'] for case in storage['cases'] if case['status'] != 'PASS'] == ['real-enospc'],
                'unexpected storage skip or failure')
        require(volume['summary'] == {'PASS': 1, 'FAIL': 0, 'SKIP': 0}
                and len(volume['cases']) == 1 and volume['cases'][0]['name'] == 'real-enospc'
                and volume['cases'][0]['status'] == 'PASS', 'required ENOSPC evidence missing')
        spec = spec_from_file_location('chain_oracle', Path(__file__).with_name('run-audit-chain-campaign.py'))
        oracle = module_from_spec(spec)
        spec.loader.exec_module(oracle)
        require(len(chain['histories']) == 32
                and [item['seed'] for item in chain['histories']] == list(range(120, 152)), 'history coverage mismatch')
        with tempfile.TemporaryDirectory(prefix='mddlog-evidence-') as temporary:
            trace = Path(temporary) / 'trace.jsonl'
            prefix = PurePosixPath(manifest['reports']['chain']['archive_member']).parent
            for history in chain['histories']:
                require(history['command'][0] != 'sudo', 'chain worker unexpectedly elevated')
                trace.write_bytes(archive.read(str(prefix / f"seed-{history['seed']}" / 'trace.jsonl')))
                result = oracle.verify_trace(trace, profile['boots'], profile['records'])
                require(all(history[key] == value for key, value in result.items()), 'replayed history differs from report')
    return {'status': 'PASS', 'members': len(inventory), 'histories': 32,
            'baseline_records': sum(item['baseline_records'] for item in chain['histories']),
            'recovered_records': sum(item['recovered_records'] for item in chain['histories']),
            'scope': 'preserved bytes and bounded software histories; no physical qualification or toolchain acceptance'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, default=DEFAULT,
                        help='location of the preserved 2026-10-08 set; other campaigns are not supported')
    args = parser.parse_args()
    try:
        print(json.dumps(verify_evidence(args.evidence)))
        return 0
    # This CLI boundary reports malformed inputs and backend/codec failures consistently.
    # Process-level interrupts and argparse usage errors retain their usual behavior.
    except Exception as error:
        print(json.dumps({'status': 'FAIL', 'error': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
