"""External image integrity checks must fail for absent or inconsistent evidence."""
import csv
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "orin_archive", Path(__file__).resolve().parents[2] / "scripts/verify-orin-archive.py")
ARCHIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCHIVE)


@unittest.skipUnless(shutil.which("zstd"), "image verification requires zstd")
class OrinArchiveChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "images").mkdir()
        self.decoder = shutil.which("zstd")
        raw = b"confirmed-prefix" * 64
        self.image = self.root / "images/cycle-0001.img.zst"
        encoded = subprocess.run([self.decoder, "-c"], input=raw, capture_output=True, check=True).stdout
        self.image.write_bytes(encoded)
        self.row = {"cycle": "1", "fichier": "images/cycle-0001.img.zst",
                    "taille_partition_o": str(len(raw)), "sha256_image_brute": hashlib.sha256(raw).hexdigest(),
                    "taille_zst_o": str(len(encoded)), "sha256_zst": hashlib.sha256(encoded).hexdigest()}
        self.index = self.root / "index.csv"

    def report(self, rows=None, selected=None):
        with self.index.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=self.row.keys())
            writer.writeheader()
            writer.writerows(rows if rows is not None else [self.row])
        return ARCHIVE.verify_archive(self.index, self.root, self.decoder, selected)

    def test_valid_bytes_do_not_establish_qualification(self):
        report = self.report()
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["qualification"], "not-established")

    def test_missing_image_fails(self):
        self.image.unlink()
        self.assertEqual(self.report()["status"], "FAIL")

    def test_wrong_compressed_digest_fails(self):
        self.row["sha256_zst"] = "0" * 64
        self.assertEqual(self.report()["status"], "FAIL")

    def test_wrong_raw_digest_fails(self):
        self.row["sha256_image_brute"] = "0" * 64
        self.assertEqual(self.report()["status"], "FAIL")

    def test_truncated_and_excess_raw_bytes_fail(self):
        self.row["taille_partition_o"] = str(int(self.row["taille_partition_o"]) + 1)
        self.assertEqual(self.report()["status"], "FAIL")
        self.row["taille_partition_o"] = "1"
        self.assertEqual(self.report()["status"], "FAIL")

    def test_invalid_compression_with_matching_index_fails(self):
        invalid = b"not a zstd image"
        self.image.write_bytes(invalid)
        self.row["taille_zst_o"] = str(len(invalid))
        self.row["sha256_zst"] = hashlib.sha256(invalid).hexdigest()
        self.assertEqual(self.report()["status"], "FAIL")

    def test_external_path_and_symlink_escape_fail(self):
        self.row["fichier"] = "../other.img.zst"
        self.assertEqual(self.report()["status"], "FAIL")
        outside = self.root.parent / (self.root.name + "-outside")
        outside.write_bytes(self.image.read_bytes())
        self.addCleanup(outside.unlink)
        self.image.unlink()
        self.image.symlink_to(outside)
        self.row["fichier"] = "images/cycle-0001.img.zst"
        self.assertEqual(self.report()["status"], "FAIL")

    def test_subset_is_partial_and_duplicate_index_rejected(self):
        second = dict(self.row, cycle="2", fichier="images/cycle-0002.img.zst")
        self.assertEqual(self.report([self.row, second], {1})["status"], "PARTIAL")
        with self.assertRaises(ValueError):
            self.report([self.row, self.row])
        with self.assertRaises(ValueError):
            self.report(selected={3})

    def test_cli_refuses_missing_decoder_and_existing_report(self):
        self.report()
        script = Path(ARCHIVE.__file__)
        report = self.root.parent / (self.root.name + "-report.json")
        self.addCleanup(lambda: report.unlink(missing_ok=True))
        arguments = [sys.executable, str(script), "--index", str(self.index),
                     "--archive", str(self.root), "--output", str(report)]
        failed = subprocess.run(arguments + ["--zstd", "missing-mddlog-decoder"], capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(report.exists())
        passed = subprocess.run(arguments + ["--zstd", self.decoder], capture_output=True)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        original = report.read_bytes()
        repeated = subprocess.run(arguments + ["--zstd", self.decoder], capture_output=True)
        self.assertNotEqual(repeated.returncode, 0)
        self.assertEqual(report.read_bytes(), original)
