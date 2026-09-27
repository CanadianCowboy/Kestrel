"""Regression coverage for explicit keys and YAML merge semantics."""

import importlib.util
from pathlib import Path
import unittest

import yaml


spec = importlib.util.spec_from_file_location(
    "validate_coderabbit_config",
    Path(__file__).resolve().parents[1] / "scripts/validate_coderabbit_config.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class UniqueKeyLoaderTests(unittest.TestCase):
    def load(self, document):
        return yaml.load(document, Loader=validator.UniqueKeyLoader)

    def test_explicit_override_before_or_after_merge(self):
        for mapping in ("<<: {enabled: false}\nenabled: true",
                        "enabled: true\n<<: {enabled: false}"):
            with self.subTest(mapping=mapping):
                self.assertEqual(self.load(mapping), {"enabled": True})

    def test_nested_merges_and_reused_aliases(self):
        document = """
base: &base {enabled: false, profile: chill}
derived: &derived {<<: *base, enabled: true}
first: {<<: *derived}
second: {<<: *derived, profile: assertive}
"""
        result = self.load(document)
        self.assertEqual(result["first"], {"enabled": True, "profile": "chill"})
        self.assertEqual(result["second"], {"enabled": True, "profile": "assertive"})

    def test_merge_sequence_precedence(self):
        self.assertEqual(
            self.load("<<: [{enabled: true}, {enabled: false, profile: chill}]"),
            {"enabled": True, "profile": "chill"})

    def test_explicit_duplicates_are_rejected(self):
        for document in (
            "enabled: true\nenabled: false",
            "<<: {enabled: false}\nenabled: true\nenabled: false",
            "<<: {enabled: true, enabled: false}",
            "<<: {<<: {enabled: true, enabled: false}}",
            "items: [{enabled: true, enabled: false}]",
            "<<: {enabled: true}\n'<<': one\n'<<': two",
        ):
            with self.subTest(document=document):
                with self.assertRaisesRegex(yaml.constructor.ConstructorError,
                                            "found the key .* a second time"):
                    self.load(document)

    def test_invalid_merge_and_unhashable_key_are_rejected(self):
        for document in ("<<: scalar", "? [one, two]\n: value"):
            with self.subTest(document=document):
                with self.assertRaises(yaml.constructor.ConstructorError):
                    self.load(document)


if __name__ == "__main__":
    unittest.main()
