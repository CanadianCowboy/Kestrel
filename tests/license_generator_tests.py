"""Regression checks for the notice generator, using synthetic package metadata."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "notices", ROOT / "tools/generate-third-party-notices.py")
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class LicenseGeneratorTests(unittest.TestCase):
    def test_bundled_license_labels(self):
        for stem, expected in (
            ("certifi", "MPL-2.0"),
            ("distlib", "PSF-2.0"),
            ("packaging", "Apache-2.0 OR BSD-2-Clause"),
        ):
            paths = list((ROOT / "licenses").glob(f"*-s-src-pip--vendor-{stem}-license.txt"))
            self.assertEqual(len(paths), 1)
            body = paths[0].read_text()
            self.assertEqual(notices.text_licence("MIT", body), expected)
            self.assertEqual(notices.text_licence("NOT DECLARED", body), expected)
            self.assertTrue(paths[0].name.startswith(notices.safe_name(expected) + "-"))

    def test_gaps_follow_each_packages_copied_files(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            original_venv, original_out = notices.VENV, notices.OUT_DIR
            notices.VENV, notices.OUT_DIR = root / "packages", root / "licenses"
            notices.VENV.mkdir()
            notices.OUT_DIR.mkdir()
            try:
                for name, licence, body in (
                    ("present", "MIT", "Permission is hereby granted, free of charge"),
                    ("missing", "MIT", None),
                    ("empty", "MIT", ""),
                    ("python-dateutil", "Dual License", "Apache License\nVersion 2.0"),
                    ("undeclared-present", "", "Permission is hereby granted, free of charge"),
                    ("undeclared-missing", "", None),
                ):
                    dist = notices.VENV / f"{name}-1.0.dist-info"
                    dist.mkdir()
                    (dist / "METADATA").write_text(
                        f"Name: {name}\nVersion: 1.0\nLicense: {licence}\nLicense-File: LICENSE\n")
                    if body is not None:
                        (dist / "LICENSE").write_text(body)
                rows = notices.collect_python_packages()
                written = {}
                notices.copy_texts(rows, [], written)
                unknown, untexted = notices.licence_gaps(rows)
                self.assertEqual({r['name'] for r in unknown}, {'undeclared-missing'})
                self.assertEqual({r['name'] for r in untexted}, {'missing', 'empty'})
                # A stale copied-file record must not hide a file removed from disk.
                for path in notices.OUT_DIR.glob('*.txt'):
                    path.unlink()
                _, untexted = notices.licence_gaps(rows)
                self.assertIn('present', {r['name'] for r in untexted})
            finally:
                notices.VENV, notices.OUT_DIR = original_venv, original_out


if __name__ == '__main__':
    unittest.main()
