#!/usr/bin/env python3
"""Run the separate-authority proof in a private multi-UID user namespace."""
import argparse
import os
import signal
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def namespace_probe(command):
    """Bound namespace setup and reap its helpers before reporting unavailability."""
    with subprocess.Popen(command + ["true"], stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True, start_new_session=True) as process:
        try:
            _, diagnostic = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
            raise
        return subprocess.CompletedProcess(command, process.returncode, stderr=diagnostic)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", required=True)
    parser.add_argument("--service", required=True)
    parser.add_argument("--require-isolation", action="store_true")
    parser.add_argument("--outer-uid-start", type=int, default=100000)
    args = parser.parse_args()
    if not 1 <= args.outer_uid_start <= (2**32 - 6):
        parser.error("outer UID range must fit five nonzero UIDs")
    unshare = shutil.which("unshare")
    prefix = [unshare, "--user", "--map-auto", "--setuid", "0", "--setgid", "0"] if unshare else []
    if unshare and os.geteuid() == 0:
        prefix = [unshare, "--user", f"--map-users={args.outer_uid_start},0,5",
                  f"--map-groups={args.outer_uid_start},0,5", "--setuid", "0", "--setgid", "0"]
    try:
        probe = namespace_probe(prefix) if prefix else None
    except (OSError, subprocess.TimeoutExpired) as error:
        probe = subprocess.CompletedProcess(prefix, 1, stderr=str(error))
    if probe is None or probe.returncode:
        print("FAIL" if args.require_isolation else "SKIP", "multi-UID user namespace unavailable")
        if probe:
            print(probe.stderr.strip())
        return 1 if args.require_isolation else 77
    # Namespace UIDs must traverse the executable path even when the checkout is private.
    with tempfile.TemporaryDirectory(prefix="mddlog-witness-executables-", dir="/tmp") as staging:
        os.chmod(staging, 0o755)
        staged = []
        for name, original in (("worker", args.worker), ("service", args.service)):
            destination = Path(staging) / name
            shutil.copyfile(original, destination)
            destination.chmod(0o755)
            staged.append(str(destination))
        process = subprocess.Popen(prefix + staged, start_new_session=True)
        try:
            return process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            print("FAIL witness deployment exceeded 60 seconds")
            return 1


if __name__ == "__main__":
    sys.exit(main())
