"""Offline verification and corruption controls for the preserved #120 campaign."""
import importlib.util
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from unittest import mock

SOURCE = Path(__file__).resolve().parents[2] / 'scripts/verify-audit-evidence.py'
SPEC = importlib.util.spec_from_file_location('audit_evidence', SOURCE)
EVIDENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVIDENCE)


class AuditEvidenceTest(unittest.TestCase):
    @contextmanager
    def coherent_fixture(self, mutate):
        """Recompute all integrity fields for a small, separately trusted negative fixture.

        The fixture includes deployment logs and the first real trace. Each mutation must
        fail at its semantic check before a later trace is needed. Only the test patches
        the fixed reference to trust these fixture bytes; the production pin is unchanged.
        """
        manifest = json.loads((EVIDENCE.DEFAULT / 'manifest.json').read_text())
        reports = {kind: json.loads((EVIDENCE.DEFAULT / entry['file']).read_text())
                   for kind, entry in manifest['reports'].items()}
        mutate(reports)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            members = {}
            for kind, entry in manifest['reports'].items():
                data = (json.dumps(reports[kind]) + '\n').encode()
                (root / entry['file']).write_bytes(data)
                members[entry['archive_member']] = data
                entry.update(sha256=hashlib.sha256(data).hexdigest(), status=reports[kind]['status'])
            prefix = Path(manifest['reports']['chain']['archive_member']).parent.as_posix()
            with zipfile.ZipFile(EVIDENCE.DEFAULT / manifest['archive']['file']) as original:
                for index in range(32):
                    name = f'{prefix}/deployment-{index}.log'
                    members[name] = original.read(name)
                name = f'{prefix}/seed-120/trace.jsonl'
                members[name] = original.read(name)
            archive = root / manifest['archive']['file']
            with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as output:
                for name, data in members.items():
                    output.writestr(name, data)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            manifest['archive'].update(sha256=digest, bytes=archive.stat().st_size,
                                       entries=len(members), uncompressed_bytes=sum(map(len, members.values())))
            manifest['artifact']['digest'] = 'sha256:' + digest
            inventory = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                                for name, data in sorted(members.items())).encode()
            (root / manifest['inventory']['file']).write_bytes(inventory)
            manifest['inventory']['sha256'] = hashlib.sha256(inventory).hexdigest()
            for log in manifest['logs']:
                shutil.copyfile(EVIDENCE.DEFAULT / log['file'], root / log['file'])
            (root / 'manifest.json').write_text(json.dumps(manifest))
            with mock.patch.object(EVIDENCE, 'ORIGINAL_SHA256', digest):
                yield root

    def test_semantic_failures_after_consistent_integrity_checks(self):
        cases = (
            ('seed', 'prolonged profile mismatch'),
            ('boots', 'prolonged profile mismatch'),
            ('records', 'prolonged profile mismatch'),
            ('sudo', 'deployment isolation not required'),
            ('isolation', 'deployment isolation not required'),
            ('storage_total', 'storage campaign accounting mismatch'),
            ('storage_skip', 'unexpected storage skip or failure'),
            ('storage_cases', 'unexpected storage skip or failure'),
            ('fuzz', 'reader campaign incomplete'),
            ('coverage', 'reader campaign incomplete'),
            ('replay', 'replayed history differs from report'),
            ('worker_sudo', 'chain worker unexpectedly elevated'),
        )
        for corruption, expected in cases:
            def mutate(reports):
                chain, readers, storage = (reports[kind] for kind in ('chain', 'readers', 'storage'))
                if corruption in ('seed', 'boots', 'records'):
                    chain['profile'][corruption] += 1
                elif corruption == 'sudo':
                    chain['deployment']['commands'][0][0] = 'python3'
                elif corruption == 'isolation':
                    chain['deployment']['commands'][0].remove('--require-isolation')
                elif corruption == 'storage_total':
                    storage['summary']['PASS'] -= 1
                elif corruption == 'storage_skip':
                    next(case for case in storage['cases'] if case['status'] == 'SKIP')['name'] = 'other-skip'
                elif corruption == 'storage_cases':
                    next(case for case in storage['cases'] if case['status'] == 'PASS')['status'] = 'FAIL'
                elif corruption == 'fuzz':
                    readers['fuzz']['inputs'] -= 1
                elif corruption == 'coverage':
                    del readers['coverage']['units']['AuditStore.cppm']
                elif corruption == 'replay':
                    chain['histories'][0]['baseline_records'] += 1
                elif corruption == 'worker_sudo':
                    chain['histories'][0]['command'][0] = 'sudo'
            with self.subTest(corruption=corruption), self.coherent_fixture(mutate) as root:
                with self.assertRaisesRegex(ValueError, expected):
                    EVIDENCE.verify_evidence(root)

    def test_actual_zip_output_budget_does_not_use_declared_size(self):
        for maximum in (0, 1024):
            with self.subTest(maximum=maximum):
                source = io.BytesIO(b'x' * 65536)
                with self.assertRaisesRegex(ValueError, 'archive member exceeds decompression budget'):
                    EVIDENCE.bounded_digest(source, maximum, 'archive member')
                self.assertEqual(source.tell(), maximum + 1)

    def assert_json_failure(self, arguments):
        output, errors = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, 'argv', arguments), redirect_stdout(output), redirect_stderr(errors):
            self.assertEqual(EVIDENCE.main(), 1)
        self.assertEqual(json.loads(output.getvalue())['status'], 'FAIL')
        self.assertNotIn('Traceback', errors.getvalue())

    def test_malformed_manifest_type_is_a_json_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = json.loads((EVIDENCE.DEFAULT / 'manifest.json').read_text())
            manifest['run'] = None
            (root / 'manifest.json').write_text(json.dumps(manifest))
            self.assert_json_failure([str(SOURCE), '--evidence', str(root)])

    def test_backend_and_codec_errors_are_json_failures(self):
        for error in (TypeError('type'), IndexError('index'), zlib.error('codec'),
                      RuntimeError('encrypted ZIP'), NotImplementedError('compression'), zipfile.BadZipFile('ZIP')):
            with self.subTest(error=type(error).__name__), \
                    mock.patch.object(EVIDENCE, 'verify_evidence', side_effect=error):
                self.assert_json_failure([str(SOURCE)])

    def test_git_checkout_preserves_evidence_with_crlf_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository, export = root / 'repository', root / 'export'
            repository.mkdir()
            shutil.copyfile(SOURCE.parents[1] / '.gitattributes', repository / '.gitattributes')
            files = {'docs/validation/sample/report.json': b'{"result": "PASS"}\n',
                     'docs/validation/sample/files.sha256': b'a' * 64 + b'  report.json\n',
                     'docs/validation/sample/archive.zip': b'PK\x03\x04\0original\n',
                     'docs/validation/sample/log.gz': gzip.compress(b'original\n', mtime=0)}
            for name, data in files.items():
                path = repository / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            command = ['git', '-C', str(repository), '-c', 'core.autocrlf=true', '-c', 'core.eol=crlf']
            for arguments in (['init', '--quiet'], ['add', '.'],
                              ['checkout-index', '--all', '--prefix=' + str(export) + '/']):
                subprocess.run(command + arguments, check=True, capture_output=True)
            for name, data in files.items():
                self.assertEqual((export / name).read_bytes(), data, name)

    def test_oversized_manifest_is_rejected_before_parsing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'manifest.json').write_bytes(b' ' * 65537)
            with self.assertRaisesRegex(ValueError, 'manifest exceeds input budget'):
                EVIDENCE.verify_evidence(root)

    def test_original_archive_and_all_histories_replay_offline(self):
        result = EVIDENCE.verify_evidence(EVIDENCE.DEFAULT)
        self.assertEqual(result['members'], 8109)
        self.assertEqual(result['histories'], 32)
        self.assertEqual(result['baseline_records'], 49152)
        self.assertEqual(result['recovered_records'], 14848)

    def test_corrupt_archive_report_and_inventory_are_rejected(self):
        for name in ('audit-robustness.zip', 'chain-report.json', 'files.sha256', 'sanitizers.log.gz'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / 'evidence'
                shutil.copytree(EVIDENCE.DEFAULT, root)
                with (root / name).open('r+b') as output:
                    byte = output.read(1)
                    output.seek(0)
                    output.write(bytes([byte[0] ^ 1]))
                with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                    EVIDENCE.verify_evidence(root)

    def test_misrepresented_run_and_escaping_archive_are_rejected(self):
        for corruption in ('revision', 'path'):
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                manifest = json.loads((EVIDENCE.DEFAULT / 'manifest.json').read_text())
                if corruption == 'revision':
                    manifest['tested_revision'] = '0' * 40
                    expected = 'provenance mismatch'
                else:
                    manifest['archive']['file'] = '../outside.zip'
                    expected = 'unsafe evidence member'
                (root / 'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, expected):
                    EVIDENCE.verify_evidence(root)

    def test_replaced_archive_with_consistent_manifest_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = json.loads((EVIDENCE.DEFAULT / 'manifest.json').read_text())
            archive = root / manifest['archive']['file']
            with zipfile.ZipFile(archive, 'w') as output:
                output.writestr('truncated.txt', b'incomplete evidence')
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            manifest['archive'].update(sha256=digest, bytes=archive.stat().st_size)
            manifest['artifact']['digest'] = 'sha256:' + digest
            (root / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'original archive reference mismatch'):
                EVIDENCE.verify_evidence(root)

    def test_log_decompression_budget_rejects_self_consistent_oversized_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            content = b'x' * 1025
            data = gzip.compress(content, mtime=0)
            (root / 'log.gz').write_bytes(data)
            entry = {'file': 'log.gz', 'sha256': hashlib.sha256(data).hexdigest(),
                     'uncompressed_sha256': hashlib.sha256(content).hexdigest()}
            with self.assertRaisesRegex(ValueError, 'decompression budget'):
                EVIDENCE.verify_log(root, entry, maximum=1024)
            # The same digest is valid exactly at the accepted output boundary.
            EVIDENCE.verify_log(root, entry, maximum=1025)

    def test_replaced_workflow_logs_with_consistent_manifest_are_rejected(self):
        for name in EVIDENCE.ORIGINAL_LOGS:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / 'evidence'
                shutil.copytree(EVIDENCE.DEFAULT, root)
                manifest = json.loads((root / 'manifest.json').read_text())
                entry = next(log for log in manifest['logs'] if log['file'] == name)
                content = b'replacement workflow log\n'
                data = gzip.compress(content, mtime=0)
                (root / name).write_bytes(data)
                entry.update(sha256=hashlib.sha256(data).hexdigest(),
                             uncompressed_sha256=hashlib.sha256(content).hexdigest())
                (root / 'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, 'original workflow log reference mismatch'):
                    EVIDENCE.verify_evidence(root)

    def test_incomplete_duplicate_or_misrepresented_workflow_logs_are_rejected(self):
        for corruption in ('empty', 'missing', 'duplicate', 'extra', 'run'):
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / 'evidence'
                shutil.copytree(EVIDENCE.DEFAULT, root)
                manifest = json.loads((root / 'manifest.json').read_text())
                logs = manifest['logs']
                if corruption == 'empty':
                    logs.clear()
                elif corruption == 'missing':
                    logs.pop()
                elif corruption == 'duplicate':
                    logs[1] = logs[0].copy()
                elif corruption == 'extra':
                    logs.append(logs[0].copy())
                else:
                    logs[0]['run'] += 1
                (root / 'manifest.json').write_text(json.dumps(manifest))
                expected = 'original workflow log reference mismatch' if corruption == 'run' else 'workflow log set mismatch'
                with self.assertRaisesRegex(ValueError, expected):
                    EVIDENCE.verify_evidence(root)


if __name__ == '__main__':
    unittest.main()
