#!/usr/bin/env python3
"""Replay seeded real audit histories against an independent contract-byte/queue oracle."""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import struct
import tempfile
import time
import subprocess

from importlib.util import module_from_spec, spec_from_file_location

# Share the existing process-group timeout/output budget and provenance collection.
spec = spec_from_file_location('robustness', Path(__file__).with_name('run-audit-robustness.py'))
robustness = module_from_spec(spec)
spec.loader.exec_module(robustness)


def canonical(stream, sequence, detail):
    """ADR-004 8.2, for the campaign's explicit Lifecycle/Requested/unavailable input."""
    def string(value):
        encoded = value.encode('utf-8')
        return struct.pack('>H', len(encoded)) + encoded
    return (struct.pack('>H', 1) + string(stream) + struct.pack('>Q', sequence)
            + bytes([1, 1, 0]) + string('campaign.record') + string('')
            + string('test') + string('') + string('') + string('')
            + bytes([1]) + struct.pack('>Q', sequence) + string(detail[:160]) + bytes([int(len(detail.encode('utf-8')) > 160)]))


def verify_trace(path, boots, records):
    """The expected bytes derive from requests, never the tested decoder or encoder."""
    if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
        raise AssertionError('missing/excessive trace')
    streams = {}
    faults = set()
    complete = False
    recovered_count = 0
    for line in path.read_text().splitlines():
        event = json.loads(line)
        operation = event['op']
        if complete:
            raise AssertionError('events after completion')
        if operation == 'admit':
            stream = streams.setdefault(event['stream'], {'inputs': [], 'digests': [], 'seen': 0, 'handed': 0,
                                                         'snapshot': False, 'anchor': False, 'fault': False,
                                                         'refusals': 0, 'recovery_started': False, 'fault_health': False})
            sequence = len(stream['inputs']) + 1
            if event['sequence'] != sequence or stream['fault'] or sequence - 1 - stream['handed'] >= 4:
                raise AssertionError('admission identity mismatch')
            encoded = canonical(event['stream'], sequence, event['detail'])
            previous = stream['digests'][-1] if sequence > 1 else bytes(32)
            stream['inputs'].append(encoded)
            stream['digests'].append(hashlib.sha256(encoded + previous).digest())
        elif operation == 'refuse':
            stream = streams[event['stream']]
            if len(stream['inputs']) - stream['handed'] != 4 or stream['fault']:
                raise AssertionError('refusal does not agree with abstract capacity')
            stream['refusals'] += 1
        elif operation == 'poll':
            stream = streams[event['stream']]
            if (event['handed'] + event['pending'] != len(stream['inputs'])
                    or not stream['handed'] <= event['handed'] <= len(stream['inputs'])
                    or not 0 <= event['durable'] <= event['handed']):
                raise AssertionError('queue accounting/durable bound')
            stream['handed'] = event['handed']
        elif operation == 'record':
            stream = streams[event['stream']]
            index = stream['seen']
            if (index >= len(stream['inputs']) or bytes.fromhex(event['bytes']) != stream['inputs'][index]
                    or bytes.fromhex(event['digest']) != stream['digests'][index]):
                raise AssertionError('stored bytes/hash differ from admitted input')
            stream['seen'] += 1
        elif operation == 'snapshot':
            stream = streams[event['stream']]
            if stream['snapshot'] or event['count'] != records or stream['seen'] != records:
                raise AssertionError('baseline prefix missing/duplicated')
            stream['snapshot'] = True
        elif operation == 'anchor':
            stream = streams[event['stream']]
            if (stream['anchor'] or event['position'] != records or stream['handed'] != records
                    or bytes.fromhex(event['digest']) != stream['digests'][records - 1]
                    or event['refusals'] != stream['refusals'] or event['refusals'] == 0):
                raise AssertionError('anchor exceeds independent prefix')
            stream['anchor'] = True
        elif operation == 'fault':
            stream = streams[event['stream']]
            if (stream['fault'] or not stream['snapshot'] or not stream['anchor']
                    or event['kind'] not in ('append', 'sync', 'reclaim')
                    or not 0 < event['through'] <= records):
                raise AssertionError('fault/retention boundary missing')
            stream['fault'] = True
            stream['trimmed'] = event['through']
            stream['recovered'] = event['through']
            stream['kind'] = event['kind']
            faults.add(event['kind'])
        elif operation == 'fault_health':
            stream = streams[event['stream']]
            if (not stream['fault'] or stream['fault_health']
                    or event['handed'] + event['pending'] != len(stream['inputs'])
                    or not records <= event['handed'] <= len(stream['inputs'])
                    or event['durable'] != records or event['losses'] != event['handed'] - records):
                raise AssertionError('terminal fault conceals loss or exceeds durable prefix')
            stream['fault_health'] = True
        elif operation == 'recovered':
            stream = streams[event['stream']]
            encoded = bytes.fromhex(event['bytes'])
            # An interrupted reclaim may leave whole segments of the recorded trim physically present.
            if stream['kind'] == 'reclaim' and not stream['recovery_started']:
                index = next((i for i, value in enumerate(stream['inputs']) if value == encoded), -1)
                if not 0 <= index <= stream['trimmed']:
                    raise AssertionError('reclaim recovery begins beyond its declared boundary')
                stream['recovered'] = index
            stream['recovery_started'] = True
            index = stream['recovered']
            if (index >= len(stream['inputs']) or encoded != stream['inputs'][index]
                    or bytes.fromhex(event['digest']) != stream['digests'][index]):
                raise AssertionError('recovered prefix differs from admitted history')
            stream['recovered'] += 1
            recovered_count += 1
        elif operation == 'complete':
            if event['boots'] != boots or not event['rollback'] or event['short_writes'] == 0:
                raise AssertionError('incomplete history')
            complete = True
        else:
            raise AssertionError(f'unknown trace operation: {operation}')
    if (not complete or len(streams) != boots or faults != {'append', 'sync', 'reclaim'}
            or any(not stream['fault'] or not stream['fault_health'] for stream in streams.values())
            or any(not stream['recovery_started'] for stream in streams.values())
            or any(stream.get('recovered', 0) < records for stream in streams.values())):
        raise AssertionError('required boots, faults or recovered prefix not exercised')
    return {'status': 'PASS', 'streams': len(streams), 'baseline_records': boots * records,
            'recovered_records': recovered_count, 'faults': sorted(faults),
            'trace_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'oracle': 'ADR-004 contract bytes, abstract queue, Python hashlib.sha256'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--seed', type=int, default=120)
    parser.add_argument('--seeds', type=int, default=2)
    parser.add_argument('--boots', type=int, default=3)
    parser.add_argument('--records', type=int, default=32)
    parser.add_argument('--outage', type=int, default=16)
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--deployment-worker', type=Path)
    parser.add_argument('--witness-service', type=Path)
    parser.add_argument('--deployment-repetitions', type=int, default=1)
    parser.add_argument('--deployment-sudo', action='store_true',
                        help='elevate only the separate-authority deployment subprocess with noninteractive sudo')
    parser.add_argument('--tools-cli', type=Path)
    parser.add_argument('--tool-reference', type=Path)
    args = parser.parse_args()
    if (not 1 <= args.seed <= 2147483647 - args.seeds or not 1 <= args.seeds <= 32
            or not 3 <= args.boots <= 24 or not 32 <= args.records <= 64
            or not 1 <= args.outage <= 1000 or not 1 <= args.timeout <= 600):
        parser.error('profile exceeds bounded campaign limits')
    if (args.deployment_sudo and not args.deployment_worker
            or bool(args.deployment_worker) != bool(args.witness_service)
            or bool(args.tools_cli) != bool(args.tool_reference)
            or not 1 <= args.deployment_repetitions <= 32):
        parser.error('integration binaries must be supplied in pairs; repetitions must be 1..32')
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    started = time.monotonic()
    report = {'schema': 1, 'status': 'FAIL', 'profile': vars(args).copy(), 'histories': [],
              'environment': {'uname': platform.uname()._asdict(), 'python': platform.python_version()},
              'scope': 'real-file witness functional faults; shared UID, no independent custody qualification'}
    report['deployment'] = {'status': 'NOT_EXECUTED'}
    report['tools'] = {'status': 'NOT_EXECUTED'}
    report['sanitizer_options'] = {key: os.environ.get(key, 'runtime default')
                                   for key in ('ASAN_OPTIONS', 'UBSAN_OPTIONS', 'TSAN_OPTIONS')}
    report['profile'] = {key: str(value) if isinstance(value, Path) else value for key, value in report['profile'].items()}
    try:
        worker = args.worker.resolve()
        report.update(robustness.repository_metadata(Path(__file__).resolve().parents[1]))
        repository = Path(__file__).resolve().parents[1]
        report['source_sha256'] = {name: hashlib.sha256((repository / name).read_bytes()).hexdigest()
                                   for name in ('scripts/run-audit-chain-campaign.py', 'scripts/run-audit-robustness.py',
                                                'tests/spec/AuditChainCampaign.cpp', 'scripts/run-witness-deployment.py',
                                                'scripts/run-audit-tools.py',
                                                'tests/archives/audit-export/GenerateV02.cpp',
                                                'tests/archives/audit-export/GenerateV03.cpp')}
        report['build'] = robustness.build_metadata(worker)
        report['binary_sha256'] = hashlib.sha256(worker.read_bytes()).hexdigest()
        for seed in range(args.seed, args.seed + args.seeds):
            history = directory / f'seed-{seed}'
            history.mkdir()
            command = [str(worker), str(history), str(seed), str(args.boots), str(args.records), str(args.outage)]
            result = {'seed': seed, 'command': command, 'status': 'FAIL'}
            report['histories'].append(result)
            robustness.run_logged(command, history / 'worker.log', args.timeout)
            result.update(verify_trace(history / 'trace.jsonl', args.boots, args.records))
        scripts = Path(__file__).resolve().parent
        if args.deployment_worker:
            result = {'status': 'FAIL', 'commands': [], 'repetitions': args.deployment_repetitions,
                      'binary_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                        for path in (args.deployment_worker, args.witness_service)}}
            report['deployment'] = result
            for index in range(args.deployment_repetitions):
                command = ['python3', str(scripts / 'run-witness-deployment.py'),
                           '--worker', str(args.deployment_worker.resolve()),
                           '--service', str(args.witness_service.resolve()), '--require-isolation']
                if args.deployment_sudo:
                    command = ['sudo', '--non-interactive', '--preserve-env=ASAN_OPTIONS,UBSAN_OPTIONS', *command]
                result['commands'].append(command)
                robustness.run_logged(command, directory / f'deployment-{index}.log', 75)
            result['status'] = 'PASS'
        if args.tools_cli:
            corpora = sorted((scripts.parent / 'tests/archives/audit-export').glob('v*.json'))
            if not corpora or len(corpora) > 16:
                raise AssertionError('archive corpus missing/exceeds release budget')
            report['tools'] = {'status': 'FAIL', 'commands': [],
                               'corpus_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in corpora},
                               'binary_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                                 for path in (args.tools_cli, args.tool_reference)}}
            for corpus in corpora:
                command = ['python3', str(scripts / 'run-audit-tools.py'), '--cli', str(args.tools_cli.resolve()),
                           '--reference', str(args.tool_reference.resolve()), '--corpus', str(corpus)]
                report['tools']['commands'].append(command)
                robustness.run_logged(command, directory / (corpus.stem + '-tools.log'), 75)
            report['tools']['status'] = 'PASS'
        report['status'] = 'PASS'
    except (AssertionError, OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        report['duration_seconds'] = time.monotonic() - started
        destination = directory / 'report.json'
        destination.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'status': report['status'], 'report': str(destination)}))
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
