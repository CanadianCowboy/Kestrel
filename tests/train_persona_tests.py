import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("train_persona", Path(__file__).parents[1] / "scripts/train_persona.py")
trainer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trainer)


class Tokenizer:
    eos_token_id = 0

    @staticmethod
    def encode(text, add_special_tokens=False):
        return list(text.encode())


class TrainingDataTests(unittest.TestCase):
    def record(self, prompt="hello", preferred="hi"):
        return dict(schema_version=1, reviewed=True, base_model=trainer.BASE_MODEL,
                    prompt=prompt, preferred=preferred, rejected="formal greeting")

    def load(self, records):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reviewed.jsonl"
            path.write_text("\n".join(json.dumps(item) for item in records))
            return trainer.load_examples(path)

    def test_review_and_base_identity_are_required(self):
        for change in [dict(reviewed=False), dict(base_model="Qwen/Qwen3-4B-Instruct-2507"), dict(schema_version=2)]:
            with self.assertRaises(ValueError):
                self.load([{**self.record(), **change}, self.record("bye", "later")])

    def test_invalid_or_duplicate_examples_are_refused(self):
        for records in [[self.record()], [self.record(), self.record()],
                        [self.record(preferred=""), self.record("bye", "later")],
                        [self.record(prompt="a" * 4001), self.record("bye", "later")]]:
            with self.assertRaises(ValueError):
                self.load(records)

    def test_split_is_reproducible_and_prompt_disjoint(self):
        examples = self.load([self.record("hello", "hi"), self.record("hello", "hey"), self.record("bye", "later")])
        train, validation = trainer.split_examples(examples)
        self.assertTrue(train and validation)
        self.assertFalse({item["prompt"] for item in train} & {item["prompt"] for item in validation})
        self.assertEqual((train, validation), trainer.split_examples(examples))

    def test_only_preferred_answer_contributes_loss(self):
        result = trainer.encode_example(Tokenizer(), self.record(), 128)
        prefix = b"user: hello\nassistant:"
        self.assertEqual(result["input_ids"][:len(prefix)], list(prefix))
        self.assertEqual(result["labels"][:len(prefix)], [-100] * len(prefix))
        self.assertEqual(result["labels"][len(prefix):], list(b" hi") + [0])
        self.assertNotIn(b"formal greeting", bytes(result["input_ids"]))

    def test_oversized_token_sequences_are_not_silently_truncated(self):
        with self.assertRaises(ValueError):
            trainer.encode_example(Tokenizer(), self.record(), 8)


if __name__ == "__main__":
    unittest.main()
