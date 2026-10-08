#!/usr/bin/env python3
"""Exercise the installed-style CLI from an independent process and decode JSON externally."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time


def number(value):
    return struct.pack('<Q', value)


def blob(value):
    return number(len(value)) + value


def text(value):
    return blob(value.encode('utf-8'))


def corpus_package(corpus):
    """Independent v1 encoder around original v0.2 bytes, not a re-encoding of events."""
    listing = text(corpus['providerId']) + number(corpus['head']) + number(len(corpus['anchors']))
    for anchor in corpus['anchors']:
        listing += number(0) + number(anchor['anchorFormat']) + number(anchor['canonicalVersion'])
        listing += text(anchor['streamId']) + number(anchor['position']) + bytes.fromhex(anchor['digest'])
        listing += text(corpus['providerId']) + number(anchor['counter']) + number(0) + number(0)
    profile = [4096, 1048576, 67108864, 65536, 134217728, 4096, 262144, 4096, 1024, 32768, 4096, 0]
    payload = b'MDDAUDIT' + number(1) + text('v0.2.0 corpus') + text('original v0.2 test witness; accepted assumption')
    payload += blob(number(0) + number(0)) + number(0) + b''.join(map(number, profile))
    payload += blob(listing) + number(0) + number(0) + number(len(corpus['segments']))
    for segment in corpus['segments']:
        payload += number(segment['reference']) + blob(bytes.fromhex(segment['hex']))
    return payload + hashlib.sha256(payload).digest()


def crc32c(data):
    state = 0xffffffff
    for byte in data:
        state ^= byte
        for _ in range(8):
            state = (state >> 1) ^ (0x82f63b78 if state & 1 else 0)
    return state ^ 0xffffffff


def run(args, expected=0):
    answer = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=15)
    if answer.returncode != expected:
        raise AssertionError(f'{args}: expected {expected}, got {answer.returncode}\n{answer.stdout}\n{answer.stderr}')
    return answer


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'reference', 'corpus'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mddlog-audit-tools-') as temp:
        root = Path(temp)
        journal, authority, retained = [root / name for name in ('journal', 'authority', 'reader')]
        for directory in (journal, authority, retained):
            directory.mkdir(mode=0o700)
        run([args.reference, 'create', journal, authority])
        oracle = json.loads(run([args.reference, 'report', journal, authority]).stdout)
        originals = {path.name: path.read_bytes() for path in journal.iterdir()}
        socket = root / 'witness.sock'
        uid = os.getuid()
        service = subprocess.Popen([str(args.reference), 'serve', str(socket), str(authority)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not socket.exists():
                if service.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError('witness startup failed')
                time.sleep(0.01)
            trust = ['--socket', socket, '--provider-id', 'cli-witness', '--server-uid', uid]
            inspect = [args.cli, 'inspect', '--source', journal, '--format', 'json']
            actual = json.loads(run(inspect + trust).stdout)
            assert actual['report'] == oracle['report'], 'CLI differs from direct LogVerifier on real files'
            assert actual['selection']['complete'] and actual['trust'] == 'authenticated-unix-provider'
            event = next(event for event in actual['events'] if event['streamId'] == 'cli/alpha')
            assert event['fields']['detail'].encode('latin1').decode('utf8') == 'quote " newline\n UTF8 é'
            assert bytes.fromhex(event['canonicalHex']), 'external consumer reads canonical bytes'
            run([args.cli, 'checkpoint-init', '--retained', retained, '--provider-id', 'cli-witness'])
            checkpoint = (retained / 'retained.bin').read_bytes()
            run(inspect + trust + ['--retained', retained])
            assert (retained / 'retained.bin').read_bytes() == checkpoint, 'inspection silently advanced reader'
            run(inspect + trust + ['--retained', retained, '--update-retained'])
            assert (retained / 'retained.bin').read_bytes() != checkpoint
            checkpoint = (retained / 'retained.bin').read_bytes()
            bounded = json.loads(run(inspect + trust + ['--retained', retained, '--max-records', '1'], 2).stdout)
            assert bounded['report']['resourceIssueId'] != 0 and not bounded['report']['streams']
            assert (retained / 'retained.bin').read_bytes() == checkpoint
            run(inspect + trust + ['--retained', root / 'missing-reader'], 2)
            run(inspect + trust + ['--max-provider-entries', '1'], 2)
            run(inspect + ['--max-archive-bytes', '1'], 2)
            aged = json.loads(run(inspect + trust + ['--max-anchor-age-ns', '0'], 4).stdout)
            assert all(stream['report']['ageId'] == 3 for stream in aged['report']['streams'])
            stale = json.loads(run(inspect + trust + ['--max-anchor-age-ns', '0', '--verification-time-ns', str(2**63 - 1)], 4).stdout)
            assert all(stream['report']['ageId'] == 2 for stream in stale['report']['streams'])
            unknown_target = next(journal.glob('*.mdl'))
            original_layout = unknown_target.read_bytes()
            unknown_layout = bytearray(original_layout)
            unknown_layout[5] = 2
            unknown_target.write_bytes(unknown_layout)
            unsupported = json.loads(run(inspect + trust, 2).stdout)
            assert unsupported['report']['unknownLayoutVersion'] == 2
            unknown_target.write_bytes(original_layout)
            run([args.cli, 'export', '--source', journal, '--output', journal / 'forbidden.mda'] + trust, 2)
            assert not (journal / 'forbidden.mda').exists(), 'export polluted journal inventory'
            package = root / 'reference.mda'
            run([args.cli, 'export', '--source', journal, '--output', package] + trust)
            assert package.stat().st_mode & 0o777 == 0o600
            verify = [args.cli, 'verify', '--archive', package, '--format', 'json']
            replay = json.loads(run(verify + ['--accept-embedded-provider']).stdout)
            assert replay['report'] == actual['report'] and replay['events'] == actual['events']
            assert replay['trust'] == 'accepted-embedded-assumption'
            untrusted = json.loads(run(verify, 4).stdout)
            assert all(stream['report']['verdictId'] == 9 for stream in untrusted['report']['streams'])
            assert json.loads(run(verify + trust).stdout)['report'] == actual['report']
            filtered = root / 'filtered.json'
            run([args.cli, 'export', '--source', journal, '--format', 'json', '--stream', 'cli/alpha', '--output', filtered] + trust)
            selection = json.loads(filtered.read_text())
            assert not selection['selection']['complete'] and all(event['streamId'] == 'cli/alpha' for event in selection['events'])
            assert selection['report'] == actual['report'], 'filter changed full-log coverage'
            run([args.cli, 'verify', '--archive', filtered], 2)
            run([args.cli, 'export', '--source', journal, '--output', root / 'bad.mda', '--stream', 'cli/alpha'], 2)
            run([args.cli, 'export', '--source', journal, '--output', package] + trust, 2)
            run(inspect + ['--socket', root / 'missing.sock', '--provider-id', 'cli-witness', '--server-uid', uid], 4)
            run(inspect + trust + ['--server-uid', uid], 2)  # duplicate named option
            run(inspect + ['--max-total-bytes', '1'], 2)
            run([args.cli, 'verify', '--archive', package, '--max-archive-bytes', '1'], 2)
            data = package.read_bytes()
            for name, damaged in [('cut', data[:-1]), ('unknown', data[:8] + number(2) + data[16:]), ('checksum', data[:-1] + bytes([data[-1] ^ 1]))]:
                path = root / (name + '.mda')
                path.write_bytes(damaged)
                run([args.cli, 'verify', '--archive', path], 2)
            config = root / 'inspect.conf'
            config.write_text(f'version=1\nsource={journal}\nformat=json\nsocket={socket}\nprovider-id=cli-witness\nserver-uid={uid}\n')
            assert json.loads(run([args.cli, 'inspect', '--config', config]).stdout)['report'] == actual['report']
            for contents in ('version=2\n', 'version=1\nunknown=true\n', 'version=1\nformat=json\nformat=human\n'):
                config.write_text(contents)
                run([args.cli, 'inspect', '--config', config], 2)
            # Change canonical bytes without changing the independently held witness.
            target = next(path for path in journal.glob('*.mdl') if b'inventory.inspect' in path.read_bytes())
            changed = bytearray(target.read_bytes())
            changed_at = changed.index(b'inventory.inspect')
            changed[changed_at] = ord('I')
            frame = 6
            while frame < len(changed):
                payload_size = struct.unpack('>I', changed[frame + 1:frame + 5])[0]
                crc_at = frame + 5 + payload_size
                if frame <= changed_at < crc_at:
                    changed[crc_at:crc_at + 4] = struct.pack('>I', crc32c(changed[frame:crc_at]))
                    break
                frame = crc_at + 4
            target.write_bytes(changed)
            altered = json.loads(run(inspect + trust, 3).stdout)
            assert any(stream['report']['verdictId'] == 0 for stream in altered['report']['streams'])
            target.write_bytes(originals[target.name])
            assert {path.name: path.read_bytes() for path in journal.iterdir()} == originals, 'CLI changed original journal files'
        finally:
            service.terminate()
            service.communicate(timeout=5)
        corpus = json.loads(args.corpus.read_text())
        old = root / 'v02.mda'
        old.write_bytes(corpus_package(corpus))
        imported = json.loads(run([args.cli, 'verify', '--archive', old, '--accept-embedded-provider', '--format', 'json']).stdout)
        assert len(imported['report']['streams']) == 2 and all(stream['report']['verdictId'] == 7 for stream in imported['report']['streams'])
        assert len([event for event in imported['events'] if event['streamId'] == 'v02/producer']) == 3
        assert all(event['canonicalVersion'] == 1 for event in imported['events'])
        # Same old bytes as a real Linux directory give the same report as packaged replay.
        legacy = root / 'legacy'
        legacy.mkdir(mode=0o700)
        for segment in corpus['segments']:
            path = legacy / f"{segment['reference']:016x}.mdl"
            path.write_bytes(bytes.fromhex(segment['hex']))
            path.chmod(0o600)
        unanchored = json.loads(run([args.cli, 'inspect', '--source', legacy, '--format', 'json'], 4).stdout)
        assert len(unanchored['events']) == len(imported['events'])
        assert all(stream['report']['verdictId'] == 9 for stream in unanchored['report']['streams'])
    print('audit.tools: real-file oracle, replay, trust, checkpoints, filters, budgets, errors and v0.2 corpus passed')


if __name__ == '__main__':
    main()
