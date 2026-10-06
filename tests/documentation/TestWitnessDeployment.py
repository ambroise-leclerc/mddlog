"""An unavailable namespace must skip optional tests and fail mandatory proofs."""

import importlib.util
import io
from pathlib import Path
import signal
import subprocess
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "witness_deployment", Path(__file__).resolve().parents[2] / "scripts/run-witness-deployment.py")
DEPLOYMENT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPLOYMENT)


class WitnessDeploymentNegativeControls(unittest.TestCase):
    def test_probe_unavailability_never_passes_or_runs_worker(self):
        for error in [subprocess.TimeoutExpired(["unshare"], 10), OSError("namespace unavailable")]:
            for required, expected, label in [(False, 77, "SKIP"), (True, 1, "FAIL")]:
                with self.subTest(error=type(error).__name__, required=required):
                    arguments = ["runner", "--worker", "/worker", "--service", "/service"]
                    if required:
                        arguments.append("--require-isolation")
                    output = io.StringIO()
                    with mock.patch.object(DEPLOYMENT.sys, "argv", arguments), \
                         mock.patch.object(DEPLOYMENT.sys, "stdout", output), \
                         mock.patch.object(DEPLOYMENT.shutil, "which", return_value="/unshare"), \
                         mock.patch.object(DEPLOYMENT, "namespace_probe", side_effect=error), \
                         mock.patch.object(DEPLOYMENT.subprocess, "Popen") as worker:
                        self.assertEqual(DEPLOYMENT.main(), expected)
                        worker.assert_not_called()
                    self.assertTrue(output.getvalue().startswith(label + " multi-UID"))

    def test_probe_timeout_kills_helper_group_and_reaps_it(self):
        process = mock.MagicMock()
        process.pid = 1234
        process.communicate.side_effect = [subprocess.TimeoutExpired(["unshare"], 10), (None, "")]
        with mock.patch.object(DEPLOYMENT.subprocess, "Popen") as launch, \
             mock.patch.object(DEPLOYMENT.os, "killpg") as kill:
            launch.return_value.__enter__.return_value = process
            with self.assertRaises(subprocess.TimeoutExpired):
                DEPLOYMENT.namespace_probe(["unshare", "--user"])
            self.assertTrue(launch.call_args.kwargs["start_new_session"])
            kill.assert_called_once_with(process.pid, signal.SIGKILL)
            self.assertEqual(process.communicate.call_args_list, [mock.call(timeout=10), mock.call()])
