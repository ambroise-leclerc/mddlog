#!/usr/bin/env python3
"""Observed SIGKILL, real permissions and private bounded-volume tests for #114.

No filesystem error is injected here. Short transfers at named stop points use the
test-only worker's syscall seam; power loss and protected-reader rollback need a
separately provisioned deployment. See docs/file-storage-test-plan.md.
"""

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import selectors
import signal
import subprocess
import sys
import tempfile
import time


BASELINE = b"confirmed-prefix:114:v1"
INITIAL = b"target-prefix:114:v1"
PENDING = b"pending-bytes:114:v1"
POINTS = {
    "open": ["open.before", "metadata.partial", "metadata.written",
             "metadata.file.before", "metadata.file.after", "metadata.rename.before",
             "metadata.rename.after", "metadata.dir.before", "metadata.dir.after",
             "segment.partial", "segment.written", "open.acknowledged"],
    "append": ["append.before", "append.partial", "append.written", "append.acknowledged"],
    "sync": ["sync.file.before", "sync.file.after", "sync.dir.before", "sync.dir.after", "sync.acknowledged"],
    "reclaim": ["reclaim.before", "reclaim.unlinked", "reclaim.dir.after", "reclaim.acknowledged"],
    "read": ["read.partial"],
}
RESIDUAL = {"metadata.partial", "metadata.written", "metadata.file.before",
            "metadata.file.after", "metadata.rename.before"}


def segment_name(ref):
    return f"{ref:016x}.mdl"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def execute(command, timeout=15, **kwargs):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True, **kwargs)
    buffers = {process.stdout: bytearray(), process.stderr: bytearray()}
    deadline = time.monotonic() + timeout
    try:
        with selectors.DefaultSelector() as selector:
            for stream in buffers:
                selector.register(stream, selectors.EVENT_READ)
            while selector.get_map():
                require(time.monotonic() < deadline, f"command timed out: {command}")
                for key, _ in selector.select(timeout=0.1):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    buffer = buffers[key.fileobj]
                    buffer.extend(chunk)
                    limit = 2 * 1024 * 1024 if key.fileobj is process.stdout else 65536
                    require(len(buffer) <= limit, "worker output exceeds campaign budget")
        code = process.wait(timeout=max(0.01, deadline - time.monotonic()))
        require(code == 0, f"{command}: exit {code}\n{buffers[process.stderr].decode(errors='replace')[-8192:]}")
        return [json.loads(line) for line in buffers[process.stdout].splitlines() if line]
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)
        for stream in buffers:
            stream.close()


def worker(binary, directory, operation, **kwargs):
    return execute([str(binary), str(directory), operation], **kwargs)


def result(events, kind="operation"):
    matches = [event for event in events if event["event"] == kind]
    require(len(matches) == 1, f"expected one {kind} event: {events}")
    return matches[0]


def observed_kill(binary, directory, operation, point):
    """Kill only after the requested point is observed; never substitute a timer."""
    process = subprocess.Popen([str(binary), str(directory), operation, point],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    events, pending, received = [], b"", 0
    deadline = time.monotonic() + 10
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while time.monotonic() < deadline:
                if not selector.select(timeout=min(0.1, max(0, deadline - time.monotonic()))):
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                require(chunk, f"worker exited before {point}")
                received += len(chunk)
                pending += chunk
                require(received <= 65536, "checkpoint output exceeds campaign budget")
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    event = json.loads(line)
                    events.append(event)
                    if event.get("event") == "checkpoint":
                        require(event.get("point") == point, "wrong interruption point")
                        os.killpg(process.pid, signal.SIGKILL)
                        _, stderr = process.communicate(timeout=5)
                        require(process.returncode == -signal.SIGKILL, "worker was not killed by SIGKILL")
                        require(not stderr, f"worker diagnostics: {stderr[-8192:]!r}")
                        return events
            raise AssertionError(f"checkpoint not reached within deadline: {point}")
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()


def check_inventory(binary, directory):
    events = worker(binary, directory, "inspect")
    require(not any(e["event"] == "startup-failed" for e in events), "unexpected startup rejection")
    actual = {e["ref"]: bytes.fromhex(e["hex"]) for e in events if e["event"] == "segment"}
    expected = {int(path.stem, 16): path.read_bytes() for path in directory.glob("*.mdl")}
    require(actual == expected, "reader differs from independently observed filesystem bytes")
    require(result(events, "inventory")["count"] == len(expected), "inventory count differs")
    return events


def crash_case(binary, directory, operation, point):
    require(result(worker(binary, directory, "seed"))["durable"], "seed barriers were not acknowledged")
    evidence = {"operation": operation, "point": point,
                "events": observed_kill(binary, directory, operation, point)}
    if point.endswith(".acknowledged"):
        acknowledged = result(evidence["events"])
        require(acknowledged["ok"], "checkpoint did not acknowledge a successful operation")
        if operation == "sync":
            require(acknowledged["durable"], "sync checkpoint did not acknowledge Durable")
        if operation == "open":
            require(acknowledged["ref"] == 3, "opening checkpoint did not expose the expected reference")
    require((directory / segment_name(1)).read_bytes() == BASELINE, "confirmed baseline changed")
    target = directory / segment_name(2)
    if operation == "reclaim" and point != "reclaim.before":
        require(not target.exists(), "observed unlink did not remove the target")
    else:
        content = target.read_bytes()
        require(content.startswith(INITIAL), "confirmed target prefix changed")
        suffix = content[len(INITIAL):]
        if operation == "sync" or point in {"append.written", "append.acknowledged"}:
            require(suffix == PENDING, "observed complete append is missing")
        elif point == "append.partial":
            require(suffix and len(suffix) < len(PENDING) and PENDING.startswith(suffix), "partial append is not a proper prefix")
        else:
            require(not suffix, "unexpected append before its write")
    new_segment = directory / segment_name(3)
    if point in {"segment.partial", "segment.written", "open.acknowledged"}:
        content = new_segment.read_bytes()
        require(content and PENDING.startswith(content), "new segment is not the intended prefix")
        if point != "segment.partial":
            require(content == PENDING, "observed complete opening is missing")
    else:
        require(not new_segment.exists(), "segment created before reservation completed")
    if point in RESIDUAL:
        require((directory / ".mddlog-refs.tmp").exists(), "reservation residue was lost")
        rejected = result(worker(binary, directory, "inspect"), "startup-failed")
        require(rejected["inventory"] and rejected["errno"] == 0, "incomplete reservation was silently resumed")
        evidence["restart"] = rejected
    else:
        require(not (directory / ".mddlog-refs.tmp").exists(), "unexpected temporary reservation")
        evidence["restart"] = check_inventory(binary, directory)
        reserved = int((directory / ".mddlog-refs").read_text()[8:24], 16)
        reopened = result(worker(binary, directory, "open"))
        require(reopened["ok"] and reopened["ref"] == reserved + 1, "reopening reused a reserved reference")
        evidence["next_ref"] = reopened["ref"]
    return evidence


def without_root():
    os.setgroups([])
    os.setgid(65534)
    os.setuid(65534)


def permission_case(binary, directory, operation):
    options = {}
    if os.geteuid() == 0:
        os.chown(directory, 65534, 65534)
        options["preexec_fn"] = without_root
    # A separate /tmp directory permits traversal by a dropped-privilege worker.
    require(result(worker(binary, directory, "seed", **options))["ok"], "permission seed failed")
    if operation in {"open", "reclaim"}:
        directory.chmod(0o500)
    else:
        (directory / segment_name(2)).chmod(0 if operation == "read" else 0o400)
    try:
        failure = result(worker(binary, directory, operation, **options))
        require(not failure["ok"] and failure["errno"] == errno.EACCES, "kernel permission denial not observed")
        if operation in {"open", "reclaim"}:
            require(failure["stopped"], "failed mutation did not stop the writer")
    finally:
        directory.chmod(0o700)
        (directory / segment_name(2)).chmod(0o600)
    require((directory / segment_name(1)).read_bytes() == BASELINE, "baseline changed after denial")
    require((directory / segment_name(2)).read_bytes() == INITIAL, "target changed after denial")
    return {"operation": operation, "result": failure, "worker_uid": 65534 if options else os.geteuid()}


def corrupt_counter(binary, directory, content):
    require(result(worker(binary, directory, "seed"))["ok"], "counter seed failed")
    (directory / ".mddlog-refs").write_bytes(content)
    rejected = result(worker(binary, directory, "inspect"), "startup-failed")
    require(rejected["inventory"] and rejected["errno"] == 0, "corrupt counter accepted or false errno")
    require((directory / segment_name(1)).read_bytes() == BASELINE, "corruption test changed the baseline")
    return {"counter_hex": content.hex(), "restart": rejected}


def resource_case(binary, directory):
    require(result(worker(binary, directory, "seed"))["ok"], "resource seed failed")
    evidence = result(worker(binary, directory, "resources"), "resources")
    require(evidence["before"] == evidence["after"] and evidence["last_ref"] == 66, "resource or identity growth was not controlled")
    check_inventory(binary, directory)
    require((directory / segment_name(1)).read_bytes() == BASELINE and (directory / segment_name(2)).read_bytes() == INITIAL, "resource campaign changed confirmed bytes")
    return evidence


def volume_child(binary, mountpoint):
    """Already in a private mount namespace; never fill an existing host volume."""
    mountpoint.mkdir()
    try:
        mounted = subprocess.run(["mount", "-t", "tmpfs", "-o", "size=1m,nr_inodes=256,mode=0700", "tmpfs", str(mountpoint)],
                                 capture_output=True, text=True, timeout=10)
    except OSError as error:
        return {"status": "SKIP", "reason": str(error), "tests": []}
    if mounted.returncode:
        return {"status": "SKIP", "reason": f"private tmpfs mount unavailable: {mounted.stderr.strip()}", "tests": []}
    require(os.stat(mountpoint).st_dev != os.stat(mountpoint.parent).st_dev, "mount isolation check failed")
    capacity = os.statvfs(mountpoint)
    require(capacity.f_blocks * capacity.f_frsize <= 2 * 1024 * 1024, "volume exceeds safe capacity")
    outcomes = []
    for name, operation, free_pages in [("append-data", "append-big", 0), ("reserve", "open-big", 0), ("open-data", "open-big", 1)]:
        directory = mountpoint / name
        directory.mkdir(mode=0o700)
        require(result(worker(binary, directory, "seed-big"))["durable"], "volume seed not confirmed")
        filler = mountpoint / "filler"
        allocated = 0
        with filler.open("wb", buffering=0) as output:
            try:
                for _ in range(32):
                    allocated += output.write(b"F" * 65536)
            except OSError as error:
                require(error.errno == errno.ENOSPC, "filler failed for a reason other than ENOSPC")
            else:
                raise AssertionError("private volume did not become full within budget")
        if free_pages:
            with filler.open("r+b") as output:
                output.truncate(max(0, allocated - free_pages * os.sysconf("SC_PAGESIZE")))
        try:
            failure = result(worker(binary, directory, operation))
            require(not failure["ok"] and failure["errno"] == errno.ENOSPC and failure["stopped"], "backend did not report the real ENOSPC")
            if operation == "open-big":
                require(failure["no_space"], "ENOSPC opening did not return NoSpace")
            require(not failure["durable"], "failed mutation confirmed data")
            require((directory / segment_name(1)).read_bytes() == b"B" * 4096, "confirmed baseline changed on full volume")
            target = (directory / segment_name(2)).read_bytes()
            require(target[:4096] == b"T" * 4096, "confirmed target prefix changed on full volume")
            require(target[4096:] == b"A" * (len(target) - 4096), "unexpected partial append suffix")
            if free_pages:
                partial = (directory / segment_name(3)).read_bytes()
                require(partial and len(partial) < 131072 and partial == b"N" * len(partial), "opening did not preserve the expected incomplete segment")
                require(failure["ref"] == 0, "failed opening exposed a reference")
            else:
                require(not (directory / segment_name(3)).exists(), "failed reservation exposed a new segment")
        finally:
            filler.unlink()
        if operation == "append-big" or free_pages:
            restart = check_inventory(binary, directory)
        else:
            restart = result(worker(binary, directory, "inspect"), "startup-failed")
            require(restart["inventory"], "failed reservation residue was repaired automatically")
        outcomes.append({"name": name, "operation": operation, "freed_pages": free_pages, "allocated_filler_bytes": allocated, "result": failure, "restart": restart})
    return {"volume": "private tmpfs, size=1m,nr_inodes=256", "tests": outcomes}


def volume_case(binary, output, required):
    command = ["unshare"]
    if os.geteuid() != 0:
        command += ["--user", "--map-root-user"]
    command += ["--mount", "--propagation", "private", "--fork"]
    try:
        probe = subprocess.run(command + ["true"], capture_output=True, text=True, timeout=10)
    except OSError as error:
        if required:
            raise AssertionError(f"required namespace tool unavailable: {error}") from error
        return {"status": "SKIP", "reason": str(error), "tests": []}
    if probe.returncode:
        if required:
            raise AssertionError(f"required mount namespace unavailable: {probe.stderr.strip()}")
        return {"status": "SKIP", "reason": probe.stderr.strip(), "tests": []}
    # Leave the empty host mountpoint directory as part of the run evidence. Its mount
    # disappears with the child namespace even if the campaign fails or times out.
    command += [sys.executable, str(Path(__file__).resolve()), "--worker", str(binary),
                "--volume-child", str(output / "private-volume")]
    events = execute(command, timeout=30)
    evidence = result(events, "volume")
    require(not required or evidence["status"] == "PASS", f"required volume skipped: {evidence.get('reason')}")
    return evidence


def capture(command):
    try:
        value = subprocess.run(command, capture_output=True, text=True, timeout=10)
        return {"returncode": value.returncode, "stdout": value.stdout.strip(), "stderr": value.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"error": str(error)}


def build_environment(binary):
    cache = binary.parent.parent / "CMakeCache.txt"
    values = {}
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if ":" not in line or "=" not in line or line.startswith(("#", "//")):
                continue
            key, value = line.split("=", 1)
            key = key.split(":", 1)[0]
            if key.startswith(("MDDLOG_BUILD_", "ENABLE_SANITIZER_", "CMAKE_CXX_FLAGS")) or key in {"CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_GENERATOR"}:
                values[key] = value
    compiler = values.get("CMAKE_CXX_COMPILER")
    return {"cache": values, "compiler": capture([compiler, "--version"]) if compiler else {"error": "build cache unavailable"},
            "cmake": capture(["cmake", "--version"]), "ninja": capture(["ninja", "--version"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--require-volume", action="store_true")
    parser.add_argument("--volume-only", action="store_true")
    parser.add_argument("--volume-child", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    binary = args.worker.resolve()
    require(binary.is_file(), "build the campaign worker first")
    if args.volume_child:
        print(json.dumps({"event": "volume", "status": "PASS", **volume_child(binary, args.volume_child)}))
        return 0
    require(sys.platform == "linux", "campaign requires Linux")
    require(args.output is not None and 1 <= args.iterations <= 10000, "output and iterations in [1,10000] required")
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.resolve()))
    report = {"schema": 1, "scope": "software; no power-loss qualification", "worker_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "environment": {"uname": platform.uname()._asdict(), "python": sys.version, "uid": os.geteuid(),
                              "build": build_environment(binary),
                              "revision": capture(["git", "rev-parse", "HEAD"]), "worktree": capture(["git", "status", "--porcelain"]),
                              "mount": capture(["findmnt", "--json", "--target", str(output)])},
              "iterations": args.iterations, "cases": [], "directory": str(output)}
    descriptors = len(os.listdir("/proc/self/fd"))

    def record(name, action):
        started = time.monotonic()
        try:
            evidence = action()
            entry = {"name": name, "status": evidence.pop("status", "PASS"), "evidence": evidence}
        except Exception as error:
            entry = {"name": name, "status": "FAIL", "reason": str(error)}
        entry["seconds"] = time.monotonic() - started
        report["cases"].append(entry)

    if not args.volume_only:
        for iteration in range(args.iterations):
            for operation, points in POINTS.items():
                for point in points:
                    name = f"kill-{iteration:05}-{point}"
                    directory = output / name
                    directory.mkdir(mode=0o700)
                    record(name, lambda d=directory, op=operation, p=point: crash_case(binary, d, op, p))
        for operation in ["open", "append", "read", "reclaim"]:
            # /tmp is traversable by the deliberately unprivileged subprocess.
            with tempfile.TemporaryDirectory(prefix="mddlog-permission-") as temporary:
                record(f"permission-{operation}", lambda d=Path(temporary), op=operation: permission_case(binary, d, op))
        for index, content in enumerate([b"", b"mddref2\n0000000000000002\n", b"mddref1\n0000000000000001\n", b"mddref1\nzzzzzzzzzzzzzzzz\n"]):
            directory = output / f"counter-{index}"
            directory.mkdir(mode=0o700)
            record(f"counter-{index}", lambda d=directory, c=content: corrupt_counter(binary, d, c))
        directory = output / "resources"
        directory.mkdir(mode=0o700)
        record("resources", lambda: resource_case(binary, directory))
    record("real-enospc", lambda: volume_case(binary, output, args.require_volume))
    after = len(os.listdir("/proc/self/fd"))
    report["supervisor_descriptors"] = {"before": descriptors, "after": after}
    if after != descriptors:
        report["cases"].append({"name": "supervisor-resources", "status": "FAIL", "reason": "descriptor leak"})
    statuses = [case["status"] for case in report["cases"]]
    report["status"] = "FAIL" if "FAIL" in statuses else "PARTIAL" if "SKIP" in statuses else "PASS"
    report["summary"] = {status: statuses.count(status) for status in ["PASS", "FAIL", "SKIP"]}
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"report": str(output / "report.json"), "status": report["status"], **report["summary"]}))
    return 1 if report["status"] == "FAIL" else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as failure:
        print(f"campaign: {failure}", file=sys.stderr)
        sys.exit(1)
