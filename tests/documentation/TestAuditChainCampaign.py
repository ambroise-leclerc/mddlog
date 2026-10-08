"""Negative controls for the independent real-chain oracle and campaign reports."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'scripts/run-audit-chain-campaign.py'
SPEC = importlib.util.spec_from_file_location('chain_campaign', SOURCE)
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)


class AuditChainCampaignTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def trace(self):
        events = []
        for boot, kind in enumerate(('append', 'sync', 'reclaim')):
            stream = f'producer/{boot}'
            previous = bytes(32)
            stored = []
            for sequence in range(1, 33):
                detail = f'input:{sequence}'
                events.append(dict(op='admit', stream=stream, sequence=sequence, detail=detail))
                if sequence == 4:
                    events.extend(dict(op='refuse', stream=stream) for _ in range(16))
                if sequence >= 4:
                    events.append(dict(op='poll', stream=stream, handed=sequence, pending=0, durable=sequence))
                encoded = CAMPAIGN.canonical(stream, sequence, detail)
                previous = hashlib.sha256(encoded + previous).digest()
                stored.append(dict(op='record', stream=stream, bytes=encoded.hex(), digest=previous.hex()))
            events.append(dict(op='poll', stream=stream, handed=32, pending=0, durable=32))
            events.extend(stored)
            events.append(dict(op='snapshot', stream=stream, count=32))
            events.append(dict(op='anchor', stream=stream, position=32, digest=previous.hex(), refusals=16))
            events.append(dict(op='fault', stream=stream, kind=kind, through=8))
            events.append(dict(op='fault_health', stream=stream, handed=32, pending=0, durable=32, losses=0))
            events.extend(dict(item, op='recovered') for item in stored[8:])
        events.append(dict(op='complete', boots=3, short_writes=100, rollback=True))
        return events

    def verify(self, events):
        path = self.root / 'trace.jsonl'
        path.write_text(''.join(json.dumps(item) + '\n' for item in events))
        return CAMPAIGN.verify_trace(path, 3, 32)

    def test_valid_reference_history(self):
        result = self.verify(self.trace())
        self.assertEqual(result['baseline_records'], 96)
        self.assertEqual(result['recovered_records'], 72)

    def test_contract_vector_and_detail_truncation(self):
        # Fixed contract bytes: version, stream length/name, sequence, category, phase, unavailable time.
        value = CAMPAIGN.canonical('p', 1, '')
        self.assertEqual(value.hex(), '00010001700000000000000001010100000f63616d706169676e2e7265636f7264'
                         '0000000474657374000000000000010000000000000001000000')
        self.assertEqual(CAMPAIGN.canonical('p', 1, 'x' * 161)[-1], 1)
        self.assertEqual(CAMPAIGN.canonical('p', 1, 'x' * 160)[-1], 0)

    def test_release_seeds_preserve_original_archive_bytes(self):
        repository = SOURCE.parents[1]
        for version, prefix, expected_hash in (
                ('v0.2.0', 'real-v02', 'b1d7f485eb046e6d95a1fa288e2904c7b6ad76ac7291e86d8ba393eeb39c532d'),
                ('v0.3.0', 'real-v03', '0852b676669ab7570753c61b139d16524b7e4ec1ce8c721116c73585052761e2')):
            with self.subTest(version=version):
                path = repository / 'tests/archives/audit-export' / (version + '.json')
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), expected_hash)
                corpus = json.loads(path.read_text())
                self.assertEqual(corpus['producerVersion'], version)
                self.assertEqual(len(corpus['segments']), 2)
                for segment, name in zip(corpus['segments'], ('ledger', 'producer')):
                    seed = repository / 'tests/fuzz/corpus' / (prefix + '-' + name + '.hex')
                    self.assertEqual(bytes.fromhex(seed.read_text()), bytes.fromhex(segment['hex']))

    def test_successful_exit_without_trace_is_failure(self):
        worker = self.root / 'worker'
        worker.write_text(f'#!{sys.executable}\n')
        worker.chmod(0o700)
        result = subprocess.run([sys.executable, str(SOURCE), '--worker', str(worker),
                                 '--output', str(self.root / 'results')], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        report = json.loads(Path(json.loads(result.stdout)['report']).read_text())
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(report['histories'][0]['status'], 'FAIL')

    def test_false_digest_corrupt_input_missing_recovery_and_premature_completion(self):
        for corruption in ('digest', 'input', 'recovery', 'completion', 'bound', 'fault', 'loss'):
            with self.subTest(corruption=corruption):
                events = self.trace()
                if corruption == 'digest':
                    next(item for item in events if item['op'] == 'record')['digest'] = '00' * 32
                elif corruption == 'input':
                    events[0]['detail'] = 'other input'
                elif corruption == 'recovery':
                    events = [item for item in events if item['op'] != 'recovered']
                elif corruption == 'completion':
                    events.pop()
                elif corruption == 'bound':
                    next(item for item in events if item['op'] == 'anchor')['position'] = 33
                elif corruption == 'loss':
                    next(item for item in events if item['op'] == 'fault_health')['losses'] = 1
                else:
                    next(item for item in events if item['op'] == 'fault')['through'] = 33
                with self.assertRaises(AssertionError):
                    self.verify(events)


if __name__ == '__main__':
    unittest.main()
