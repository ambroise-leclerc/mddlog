#!/usr/bin/env python3
"""Compare external Orin image files with the published index, without importing them into Git.

A successful result verifies bytes and lengths only. It does not qualify electrical
persistence, validate the oracle, or accept the campaign's documentary reservations.
"""

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BLOCK_BYTES = 1024 * 1024


def external_file(root, relative):
    name = Path(relative)
    if name.is_absolute() or ".." in name.parts:
        raise ValueError("archive reference must be relative without '..'")
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError("archive reference escapes its root")
    if not path.is_file():
        raise ValueError(f"missing external image: {relative}")
    return path


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(BLOCK_BYTES), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_image(root, row, decoder):
    path = external_file(root, row["fichier"])
    expected_size = int(row["taille_partition_o"])
    if expected_size <= 0 or int(row["taille_zst_o"]) <= 0:
        raise ValueError("invalid image size in index")
    if path.stat().st_size != int(row["taille_zst_o"]):
        raise ValueError("compressed image size differs from index")
    if digest_file(path) != row["sha256_zst"]:
        raise ValueError("compressed image SHA-256 differs from index")
    digest = hashlib.sha256()
    length = 0
    # Stream decompression: do not materialize the raw partition or fill the local disk.
    with tempfile.TemporaryFile() as errors:
        with subprocess.Popen([decoder, "-d", "-c", "--", str(path)], stdout=subprocess.PIPE, stderr=errors) as process:
            try:
                for block in iter(lambda: process.stdout.read(BLOCK_BYTES), b""):
                    length += len(block)
                    if length > expected_size:
                        raise ValueError("raw image exceeds indexed partition size")
                    digest.update(block)
                if process.wait() != 0:
                    raise ValueError("image decompression failed")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    if length != expected_size:
        raise ValueError("raw image size differs from index")
    if digest.hexdigest() != row["sha256_image_brute"]:
        raise ValueError("raw image SHA-256 differs from index")
    return {"cycle": int(row["cycle"]), "file": row["fichier"], "status": "PASS",
            "raw_bytes": length, "raw_sha256": digest.hexdigest(), "compressed_sha256": row["sha256_zst"]}


def verify_archive(index, archive, decoder, selected=None):
    with index.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    ids = [int(row["cycle"]) for row in rows]
    names = [row["fichier"] for row in rows]
    if not rows or len(set(ids)) != len(ids) or len(set(names)) != len(names):
        raise ValueError("image index must be nonempty with unique cycle identifiers and filenames")
    if selected and not selected.issubset(ids):
        raise ValueError("selected cycle absent from index")
    records = []
    for row in rows:
        if selected and int(row["cycle"]) not in selected:
            continue
        try:
            records.append(verify_image(archive, row, decoder))
        except (OSError, ValueError) as error:
            records.append({"cycle": int(row["cycle"]), "file": row["fichier"], "status": "FAIL", "reason": str(error)})
    failures = sum(record["status"] == "FAIL" for record in records)
    return {"checked_at_utc": datetime.now(timezone.utc).isoformat(),
            "verifier_sha256": digest_file(Path(__file__)),
            "index_sha256": digest_file(index), "archive": str(archive.resolve()),
            "indexed_images": len(rows), "checked_images": len(records), "failed_images": failures,
            "status": "FAIL" if failures else ("PASS" if len(records) == len(rows) else "PARTIAL"),
            "scope": "Indexed image bytes only; electrical traces, oracle and software provenance are not verified.",
            "qualification": "not-established", "images": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycle", type=int, action="append", help="Verify a subset; a successful subset is PARTIAL")
    parser.add_argument("--zstd", default="zstd")
    args = parser.parse_args()
    try:
        decoder = shutil.which(args.zstd)
        if not decoder:
            raise ValueError("zstd decoder is unavailable")
        report = verify_archive(args.index, args.archive, decoder, set(args.cycle) if args.cycle else None)
        output = args.output.resolve()
        # A report must never overwrite a published input or an external image.
        if output == args.index.resolve() or output.is_relative_to(args.archive.resolve()):
            raise ValueError("report output must be outside the external archive and distinct from the index")
        with output.open("x", encoding="utf-8") as destination:
            destination.write(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(f"{report['status']}: {report['checked_images']}/{report['indexed_images']} images, {report['failed_images']} failures")
        return 0 if report["status"] == "PASS" else 1
    except (OSError, ValueError, KeyError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
