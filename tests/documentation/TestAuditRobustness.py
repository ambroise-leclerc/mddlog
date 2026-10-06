"""Negative controls for the reproducible fuzzing campaign supervisor."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2] / 'scripts/run-audit-robustness.py'
SPEC = importlib.util.spec_from_file_location('audit_robustness', SOURCE)
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)


@unittest.skipUnless(sys.platform.startswith('linux'), 'POSIX process groups and selectors')
class AuditRobustnessTest(unittest.TestCase):
    """Check that campaign failures stay observable and metadata stays optional."""

    def setUp(self):
        """Isolate executable fixtures and campaign artifacts in a private directory."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def executable(self, name, source):
        """Create a fixture using the same Python interpreter as the test runner."""
        path = self.root / name
        path.write_text(f'#!{sys.executable}\n' + source)
        path.chmod(0o700)
        return path

    def test_independent_digest_mismatch_is_rejected(self):
        """A successful worker exit cannot hide a disagreement with hashlib."""
        binary = self.executable('wrong-sha', "import sys\nfor line in sys.stdin: print('0' * 64)\n")
        with self.assertRaisesRegex(AssertionError, 'disagrees'):
            CAMPAIGN.compare_sha256(binary, self.root, 120)

    def test_premature_successful_fuzzer_exit_is_rejected(self):
        """A fuzzer exiting successfully before its input budget must fail."""
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
        """Preserve the diagnostic output of an unsuccessful child process."""
        log = self.root / 'failed.log'
        with self.assertRaisesRegex(AssertionError, 'command failed'):
            CAMPAIGN.run_logged([sys.executable, '-c', "print('reduced failure'); exit(1)"], log, 5)
        self.assertIn('reduced failure', log.read_text())

    def test_excessive_diagnostics_fail_the_campaign(self):
        """Reject excessive child output instead of accumulating it indefinitely."""
        with self.assertRaisesRegex(AssertionError, 'exceeds 2 MiB'):
            CAMPAIGN.run_logged([sys.executable, '-c', "import sys; sys.stdout.write('x' * (3 * 1024 * 1024))"],
                                self.root / 'excessive.log', 5)

    def test_sha_campaign_passes_without_build_tools(self):
        """Missing CMake, Ninja and compiler binaries do not invalidate digests."""
        git = shutil.which('git')
        if git is None:
            self.skipTest('git is required to collect the repository snapshot')
        tools = self.root / 'path'
        tools.mkdir()
        (tools / 'git').symlink_to(git)
        build = self.root / 'build'
        (build / 'tests').mkdir(parents=True)
        compiler = build / 'missing-compiler'
        (build / 'CMakeCache.txt').write_text(f'CMAKE_CXX_COMPILER:FILEPATH={compiler}\n')
        sha = self.executable('build/tests/sha', "import sys,hashlib\nfor line in sys.stdin: print(hashlib.sha256(bytes.fromhex(line)).hexdigest())\n")
        env = os.environ.copy()
        env['PATH'] = str(tools)
        result = subprocess.run([sys.executable, str(SOURCE), '--sha-worker', str(sha),
                                 '--output', str(self.root / 'output')],
                                capture_output=True, text=True, timeout=10, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(next((self.root / 'output').glob('run-*/report.json')).read_text())
        self.assertEqual(report['status'], 'PASS')
        self.assertEqual(report['sha256']['inputs'], 268)
        for name in ('cmake_version', 'ninja_version', 'compiler_version'):
            self.assertTrue(report['build'][name].startswith('unavailable:'))
            self.assertIsNone(report['build'][name + '_exit_code'])

    def test_metadata_timeout_does_not_skip_remaining_tools(self):
        """A version-probe timeout remains metadata and the next probe still runs."""
        completed = subprocess.CompletedProcess(['ninja', '--version'], 0, stdout='1.13\n', stderr='')
        timeout = subprocess.TimeoutExpired(['cmake', '--version'], 5)
        with patch.object(CAMPAIGN.subprocess, 'run', side_effect=[timeout, completed]) as run:
            metadata = CAMPAIGN.build_metadata(self.root / 'tests/worker')
        self.assertEqual(run.call_count, 2)
        self.assertTrue(metadata['cmake_version'].startswith('unavailable:'))
        self.assertIsNone(metadata['cmake_version_exit_code'])
        self.assertEqual(metadata['ninja_version'], '1.13')
        self.assertEqual(metadata['ninja_version_exit_code'], 0)


if __name__ == '__main__':
    unittest.main()
