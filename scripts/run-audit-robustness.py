#!/usr/bin/env python3
"""Bounded, reproducible libFuzzer and independent SHA-256 campaigns for #120."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import selectors
import shutil
import signal
import subprocess
import tempfile
import time


def run_logged(command, log, timeout, env=None):
    """Save bounded child output and reap its process group on failure or timeout."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               start_new_session=True, env=env)
    output = bytearray()
    deadline = time.monotonic() + timeout
    try:
        with log.open('wb') as saved, selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                if time.monotonic() >= deadline:
                    raise AssertionError('campaign command timed out')
                for key, _ in selector.select(0.1):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    if len(output) + len(chunk) > 2 * 1024 * 1024:
                        raise AssertionError('campaign output exceeds 2 MiB')
                    output.extend(chunk)
                    saved.write(chunk)
            code = process.wait(timeout=max(0.01, deadline - time.monotonic()))
        if code != 0:
            raise AssertionError(f'command failed: {code}; see {log}')
        return output.decode(errors='replace')
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)
        process.stdout.close()


def compare_sha256(binary, directory, seed):
    """Require every seeded digest to match hashlib and retain the oracle inputs."""
    generator = random.Random(seed)
    lengths = [0, 1, 55, 56, 63, 64, 65, 127, 128, 129, 1024, 65536]
    lengths.extend(generator.randrange(4097) for _ in range(256))
    inputs = [generator.randbytes(length) for length in lengths]
    encoded = ''.join(data.hex() + '\n' for data in inputs)
    # Use an input file: a large stdin pipe could block while a child's output pipe fills.
    input_path = directory / 'sha-inputs.txt'
    input_path.write_text(encoded)
    with input_path.open('rb') as source:
        result = subprocess.run([str(binary)], stdin=source, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=30, check=True)
    expected = [hashlib.sha256(data).hexdigest() for data in inputs]
    actual = result.stdout.decode().splitlines()
    if actual != expected or result.stderr:
        raise AssertionError('SHA-256 disagrees with Python hashlib')
    (directory / 'sha-digests.json').write_text(json.dumps(expected, indent=2) + '\n')
    return {'status': 'PASS', 'inputs': len(inputs), 'seed': seed,
            'oracle': 'Python hashlib.sha256', 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}


def collect_coverage(binary, directory, tools):
    """Merge LLVM profiles and require executed coverage for each audit reader."""
    profiles = sorted(directory.glob('coverage-*.profraw'))
    if not profiles:
        raise AssertionError('missing reader coverage profiles')
    merged = directory / 'coverage.profdata'
    run_logged([str(tools / 'llvm-profdata'), 'merge', '-sparse',
                *map(str, profiles), '-o', str(merged)], directory / 'coverage-merge.log', 30)
    run_logged([str(tools / 'llvm-cov'), 'report', str(binary),
                f'-instr-profile={merged}', '-ignore-filename-regex=(tests|libc|__cmake)'],
               directory / 'coverage.txt', 30)
    exported = run_logged([str(tools / 'llvm-cov'), 'export', '-summary-only', str(binary),
                           f'-instr-profile={merged}', '-ignore-filename-regex=(tests|libc|__cmake)'],
                          directory / 'coverage.json', 30)
    summaries = {Path(item['filename']).name: item['summary']
                 for group in json.loads(exported)['data'] for item in group['files']}
    for name in ('AuditCanonical.cppm', 'AuditLayout.cppm', 'AuditLedger.cppm',
                 'AuditLog.cppm', 'AuditLogVerifier.cppm', 'AuditStore.cppm'):
        if name not in summaries or summaries[name]['regions']['covered'] == 0:
            raise AssertionError(f'missing executed coverage for {name}')
    return {'status': 'COLLECTED', 'report': 'coverage.txt',
            'units': {name: summary for name, summary in summaries.items() if name.endswith('.cppm')},
            'profile_sha256': hashlib.sha256(merged.read_bytes()).hexdigest()}


def build_metadata(binary):
    """Record build options and tool versions without requiring optional tools."""
    cache = binary.resolve().parent.parent / 'CMakeCache.txt'
    metadata = {}
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if '=' in line and ':' in line.split('=', 1)[0]:
                key = line.split(':', 1)[0]
                if key.startswith(('CMAKE_CXX_COMPILER', 'CMAKE_CXX_FLAGS', 'ENABLE_SANITIZER_', 'MDDLOG_BUILD_')) or key == 'CMAKE_BUILD_TYPE':
                    metadata[key] = line.split('=', 1)[1]
    commands = [('cmake_version', ['cmake', '--version']), ('ninja_version', ['ninja', '--version'])]
    compiler = metadata.get('CMAKE_CXX_COMPILER')
    if compiler:
        commands.append(('compiler_version', [compiler] if Path(compiler).name.lower() == 'cl.exe' else [compiler, '--version']))
    for name, command in commands:
        try:
            version = subprocess.run(command, capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.TimeoutExpired) as error:
            metadata[name] = f'unavailable: {error}'
            metadata[name + '_exit_code'] = None
            continue
        metadata[name] = (version.stdout + version.stderr).strip()
        metadata[name + '_exit_code'] = version.returncode
    return metadata


def repository_metadata(repository):
    """Collect optional Git snapshots without preventing a campaign report."""
    metadata = {}
    for name, command in [('revision', ['git', 'rev-parse', 'HEAD']),
                          ('worktree', ['git', 'status', '--porcelain'])]:
        try:
            snapshot = subprocess.run(command, cwd=repository, capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.SubprocessError) as error:
            metadata[name] = f'unavailable: {error}'
            continue
        metadata[name] = snapshot.stdout.strip() if snapshot.returncode == 0 else f'unavailable: exit {snapshot.returncode}'
    return metadata


def main():
    """Execute the configured campaign and write its result even after a failure."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fuzzer', type=Path)
    parser.add_argument('--sha-worker', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--runs', type=int, default=10000)
    parser.add_argument('--seed', type=int, default=120)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--llvm-tools', type=Path, default=Path('/usr/lib/llvm-21/bin'))
    parser.add_argument('--disable-leak-detection', action='store_true',
                        help='record LSan as disabled when unavailable under ptrace; CI keeps it enabled')
    parser.add_argument('--ephemeral', action='store_true',
                        help='remove successful SHA-only run artifacts; preserve failing runs and all fuzzing evidence')
    args = parser.parse_args()
    if not 1 <= args.runs <= 10000000 or not 1 <= args.seed < 2147483648 or not 1 <= args.timeout <= 3600:
        parser.error('runs, seed or timeout outside campaign budget')
    if args.ephemeral and args.fuzzer:
        parser.error('--ephemeral is only available for SHA-only tests')
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    started = time.monotonic()
    report = {'schema': 1, 'scope': 'audit readers fuzzing and SHA-256' if args.fuzzer else 'SHA-256 oracle',
              'environment': {'uname': platform.uname()._asdict(), 'python': platform.python_version()},
              'status': 'FAIL', 'directory': str(directory), 'commands': [],
              'leak_detection': not args.disable_leak_detection}
    repository = Path(__file__).resolve().parents[1]
    try:
        report.update(repository_metadata(repository))
        report['sha256'] = compare_sha256(args.sha_worker.resolve(), directory, args.seed)
        report['build'] = build_metadata(args.fuzzer or args.sha_worker)
        if args.fuzzer:
            corpus = directory / 'corpus'
            corpus.mkdir()
            seeds = Path(__file__).resolve().parents[1] / 'tests/fuzz/corpus'
            report['seeds'] = {}
            for source in sorted(seeds.glob('*.hex')):
                data = bytes.fromhex(source.read_text())
                if not data or len(data) > 65536:
                    raise AssertionError('empty or excessive corpus seed')
                (corpus / source.stem).write_bytes(data)
                report['seeds'][source.name] = hashlib.sha256(data).hexdigest()
            if not report['seeds']:
                raise AssertionError('missing versioned corpus')
            binary = args.fuzzer.resolve()
            binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
            command = [str(binary), str(corpus), f'-runs={args.runs}', f'-seed={args.seed}',
                       '-max_len=65536', '-rss_limit_mb=512', '-timeout=2', '-verbosity=0',
                       '-print_final_stats=1', f'-artifact_prefix={directory}/']
            report['commands'].append(command)
            env = os.environ.copy()
            env.update(ASAN_OPTIONS=f'detect_leaks={0 if args.disable_leak_detection else 1}:abort_on_error=1',
                       UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1',
                       LLVM_PROFILE_FILE=str(directory / 'coverage-%p.profraw'))
            output = run_logged(command, directory / 'fuzzer.log', args.timeout, env)
            count = next((line.rsplit(':', 1)[1].strip() for line in output.splitlines()
                          if line.startswith('stat::number_of_executed_units:')), None)
            if count is None or int(count) < args.runs:
                raise AssertionError('fuzzer did not execute the required input budget')
            report['fuzz'] = {'status': 'PASS', 'inputs': int(count), 'seed': args.seed,
                              'binary_sha256': binary_hash,
                              'final_corpus_files': len(list(corpus.iterdir()))}
            report['coverage'] = collect_coverage(binary, directory, args.llvm_tools)
        report['status'] = 'PASS'
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        report['duration_seconds'] = time.monotonic() - started
        (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        result = {'status': report['status'], 'report': str(directory / 'report.json')}
        if args.ephemeral and report['status'] == 'PASS':
            try:
                shutil.rmtree(directory)
            except OSError as error:
                result['cleanup_error'] = str(error)
            else:
                result.update(report=None, artifacts='successful SHA-only run removed')
        print(json.dumps(result))
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
