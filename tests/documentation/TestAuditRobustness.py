"""Negative controls for the reproducible fuzzing campaign supervisor."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'scripts/run-audit-robustness.py'
SPEC = importlib.util.spec_from_file_location('audit_robustness', SOURCE)
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)


@unittest.skipUnless(sys.platform.startswith('linux'), 'POSIX process groups and selectors')
class AuditRobustnessTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def executable(self, name, source):
        path = self.root / name
        path.write_text(f'#!{sys.executable}\n' + source)
        path.chmod(0o700)
        return path

    def test_independent_digest_mismatch_is_rejected(self):
        binary = self.executable('wrong-sha', "import sys\nfor line in sys.stdin: print('0' * 64)\n")
        with self.assertRaisesRegex(AssertionError, 'disagrees'):
            CAMPAIGN.compare_sha256(binary, self.root, 120)

    def test_premature_successful_fuzzer_exit_is_rejected(self):
        sha = self.executable('sha', "import sys,hashlib\nfor line in sys.stdin: print(hashlib.sha256(bytes.fromhex(line)).hexdigest())\n")
        fuzzer = self.executable('premature-fuzzer', 'pass\n')
        result = subprocess.run([sys.executable, str(SOURCE), '--sha-worker', str(sha),
                                 '--fuzzer', str(fuzzer), '--output', str(self.root / 'output')],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        report = json.loads(next((self.root / 'output').glob('run-*/report.json')).read_text())
        self.assertEqual(report['status'], 'FAIL')
        self.assertIn('required input budget', report['error'])

    def test_failing_command_keeps_its_log(self):
        log = self.root / 'failed.log'
        with self.assertRaisesRegex(AssertionError, 'command failed'):
            CAMPAIGN.run_logged([sys.executable, '-c', "print('reduced failure'); exit(1)"], log, 5)
        self.assertIn('reduced failure', log.read_text())

    def test_excessive_diagnostics_fail_the_campaign(self):
        with self.assertRaisesRegex(AssertionError, 'exceeds 2 MiB'):
            CAMPAIGN.run_logged([sys.executable, '-c', "import sys; sys.stdout.write('x' * (3 * 1024 * 1024))"],
                                self.root / 'excessive.log', 5)


if __name__ == '__main__':
    unittest.main()
