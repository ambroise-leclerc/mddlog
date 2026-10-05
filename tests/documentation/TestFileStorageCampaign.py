"""Negative controls: a broken worker or missing prerequisite must not pass a campaign."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "storage_campaign", Path(__file__).resolve().parents[2] / "scripts/run-file-storage-campaign.py")
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)


class CampaignNegativeControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def fake_worker(self, source):
        binary = self.directory / "worker"
        binary.write_text(f"#!{sys.executable}\n" + source)
        binary.chmod(0o700)
        return binary

    @unittest.skipUnless(sys.platform == "linux", "SIGKILL worker is a Linux campaign")
    def test_exit_without_checkpoint_is_failure(self):
        binary = self.fake_worker("print('{}')\n")
        with self.assertRaisesRegex(AssertionError, "exited before"):
            CAMPAIGN.observed_kill(binary, self.directory, "open", "metadata.file.after")

    @unittest.skipUnless(sys.platform == "linux", "SIGKILL worker is a Linux campaign")
    def test_wrong_checkpoint_is_failure(self):
        binary = self.fake_worker("import json\nprint(json.dumps({'event':'checkpoint', 'point':'wrong'}), flush=True)\n")
        with self.assertRaisesRegex(AssertionError, "wrong interruption"):
            CAMPAIGN.observed_kill(binary, self.directory, "open", "metadata.file.after")

    @unittest.skipUnless(sys.platform == "linux", "SIGKILL worker is a Linux campaign")
    def test_observed_checkpoint_is_followed_by_actual_kill(self):
        binary = self.fake_worker("import json, os, signal\nprint(json.dumps({'event':'checkpoint', 'point':'read.partial'}), flush=True)\nos.kill(os.getpid(), signal.SIGSTOP)\n")
        self.assertEqual(CAMPAIGN.observed_kill(binary, self.directory, "read", "read.partial"),
                         [{"event": "checkpoint", "point": "read.partial"}])

    def test_required_volume_cannot_be_skipped(self):
        denied = subprocess.CompletedProcess([], 1, "", "namespace unavailable")
        with mock.patch.object(CAMPAIGN.subprocess, "run", return_value=denied):
            with self.assertRaisesRegex(AssertionError, "required mount namespace unavailable"):
                CAMPAIGN.volume_case(self.directory / "worker", self.directory, True)
            self.assertEqual(CAMPAIGN.volume_case(self.directory / "worker", self.directory, False)["status"], "SKIP")

    @unittest.skipUnless(sys.platform == "linux", "SIGKILL worker is a Linux campaign")
    def test_loss_of_confirmed_prefix_is_failure(self):
        binary = self.fake_worker(f"""import json, sys, pathlib, os, signal
directory = pathlib.Path(sys.argv[1])
if sys.argv[2] == 'seed':
    (directory / '0000000000000001.mdl').write_bytes({CAMPAIGN.BASELINE!r})
    print(json.dumps({{'event':'operation', 'ok':True, 'durable':True}}))
else:
    (directory / '0000000000000001.mdl').write_bytes(b'damaged')
    print(json.dumps({{'event':'checkpoint', 'point':sys.argv[3]}}), flush=True)
    os.kill(os.getpid(), signal.SIGSTOP)
""")
        with self.assertRaisesRegex(AssertionError, "confirmed baseline changed"):
            CAMPAIGN.crash_case(binary, self.directory, "open", "metadata.file.after")

    @unittest.skipUnless(sys.platform == "linux", "pipe collector is a Linux campaign")
    def test_worker_output_is_bounded(self):
        binary = self.fake_worker("import sys\nsys.stdout.write('x' * (3 * 1024 * 1024))\n")
        with self.assertRaisesRegex(AssertionError, "output exceeds campaign budget"):
            CAMPAIGN.execute([str(binary)])
