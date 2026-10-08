"""Offline verification and corruption controls for the preserved #120 campaign."""
import importlib.util
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import zipfile

SOURCE = Path(__file__).resolve().parents[2] / 'scripts/verify-audit-evidence.py'
SPEC = importlib.util.spec_from_file_location('audit_evidence', SOURCE)
EVIDENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVIDENCE)


class AuditEvidenceTest(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
