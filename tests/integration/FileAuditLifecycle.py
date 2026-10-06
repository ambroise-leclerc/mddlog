"""Run two real file-backed service sessions and reject identity reuse without rewriting history."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def inventory(directory, pattern="*"):
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in directory.glob(pattern) if path.is_file()}


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="mddlog-file-service-") as name:
        directory = Path(name)
        for stream in ("demo:boot-1", "demo:boot-2"):
            old = inventory(directory, "*.mdl")
            result = subprocess.run([str(binary), str(directory), stream], capture_output=True, text=True, timeout=20)
            if result.returncode != 0:
                raise AssertionError(f"session {stream} failed: {result.stdout}\n{result.stderr}")
            if "durable position: 0" not in result.stdout or "chain verified" not in result.stdout:
                raise AssertionError("example must verify reopening and keep unqualified durability explicit")
            new = inventory(directory, "*.mdl")
            if any(new.get(path) != digest for path, digest in old.items()):
                raise AssertionError("restart changed a previous session's segment")
            if len(new) <= len(old):
                raise AssertionError("new session did not append its own segments")
        old = inventory(directory)
        duplicate = subprocess.run([str(binary), str(directory), "demo:boot-1"], capture_output=True, text=True, timeout=20)
        if duplicate.returncode == 0 or inventory(directory) != old:
            raise AssertionError("identity reuse must fail without changing stored evidence")
    print("PASS: two service sessions, read-only verification, unchanged history and identity reuse rejection")


if __name__ == "__main__":
    main()
