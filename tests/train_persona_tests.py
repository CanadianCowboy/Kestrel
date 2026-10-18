import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import hashlib
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("train_persona", Path(__file__).parents[1] / "scripts/train_persona.py")
trainer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trainer)

importer_spec = importlib.util.spec_from_file_location(
    "import_astronomy_dataset", Path(__file__).parents[1] / "scripts/import_astronomy_dataset.py")
importer = importlib.util.module_from_spec(importer_spec)
importer_spec.loader.exec_module(importer)


setup_spec = importlib.util.spec_from_file_location(
    "setup_instruct", Path(__file__).parents[1] / "scripts/setup_instruct.py")
setup = importlib.util.module_from_spec(setup_spec)
setup_spec.loader.exec_module(setup)


class InstructSetupTests(unittest.TestCase):
    def test_verifier_checks_magic_length_and_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'model.gguf'
            data = b'GGUFverified test bytes'
            path.write_bytes(data)
            record = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            setup.verify(path, record)
            for data in (b'GGUFwrong', b'not a gguf'):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    setup.verify(path, record)

    def test_failed_download_preserves_existing_files_and_removes_partial(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(setup.urllib.request, 'urlopen', side_effect=OSError('offline')):
                with self.assertRaises(OSError):
                    setup.install(root)
            self.assertEqual(list(root.iterdir()), [])
            path = root / setup.MODEL['filename']
            path.write_bytes(b'GGUFinvalid existing model')
            with self.assertRaises(ValueError):
                setup.install(root)
            self.assertEqual(path.read_bytes(), b'GGUFinvalid existing model')
            self.assertFalse(path.with_suffix('.manifest.json').exists())


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

    def test_separate_parent_and_stage_selection(self):
        tool = dict(self.record('load workspace', '<tool_call>{"name":"workspace","arguments":{}}</tool_call>'),
                    source='tool_trace', reviewed=False, chat_learning_consent=True, tool_success=True, adapter_stage='tool')
        records = [tool, {**tool, 'prompt': 'read workspace'}, self.record(),
                   {**self.record('instruct hello', 'hi'), 'base_model': trainer.INSTRUCT_MODEL}]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'mixed.jsonl'
            path.write_text('\n'.join(json.dumps(row) for row in records))
            selected = trainer.load_examples(path, True, adapter_stage='tool', select_stage=True)
            self.assertEqual(len(selected), 2)
            self.assertTrue(all(item['adapter_stage'] == 'tool' for item in selected))
            with self.assertRaises(ValueError):
                trainer.load_examples(path, True)
            records[0]['tool_success'] = False
            path.write_text('\n'.join(json.dumps(row) for row in records))
            with self.assertRaises(ValueError):
                trainer.load_examples(path, True, adapter_stage='tool', select_stage=True)
        for bad in ('prose', '<tool_call>{"name":"workspace","arguments":7}</tool_call>',
                    '<tool_call>{"arguments":{}}</tool_call>'):
            with self.assertRaises(ValueError):
                trainer.parse_tool_target(bad)

    def test_instruct_encoding_uses_parent_chat_template(self):
        class ChatTokenizer(Tokenizer):
            def apply_chat_template(self, messages, tokenize=False, add_generation_prompt=True):
                return '<chat>' + messages[0]['content'] + '</chat>assistant:'
        example = {**self.record(), 'base_model': trainer.INSTRUCT_MODEL}
        encoded = trainer.encode_example(ChatTokenizer(), example, 128)
        prefix = b'<chat>hello</chat>assistant:'
        self.assertEqual(encoded['input_ids'][:len(prefix)], list(prefix))
        self.assertEqual(encoded['labels'][:len(prefix)], [-100] * len(prefix))

    def test_chat_data_requires_explicit_run_permission_and_record_consent(self):
        records = [{**self.record(), 'reviewed': False, 'chat_learning_consent': True},
                   {**self.record('bye', 'later'), 'reviewed': False, 'chat_learning_consent': True}]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'chat.jsonl'
            path.write_text('\n'.join(json.dumps(item) for item in records))
            with self.assertRaises(ValueError):
                trainer.load_examples(path)
            self.assertEqual(len(trainer.load_examples(path, allow_consented_chat=True)), 2)
            records[0]['chat_learning_consent'] = False
            path.write_text('\n'.join(json.dumps(item) for item in records))
            with self.assertRaises(ValueError):
                trainer.load_examples(path, allow_consented_chat=True)

    def test_automatic_activation_requires_loss_and_behavior_checks(self):
        good = {'score': 0.5, 'safe_outputs': True}
        bad = {'score': 0.4, 'safe_outputs': True}
        self.assertTrue(trainer.activation_eligible(2.0, 1.9, good, good))
        self.assertFalse(trainer.activation_eligible(2.0, 2.1, good, good))
        self.assertFalse(trainer.activation_eligible(2.0, 1.9, good, bad))
        self.assertFalse(trainer.activation_eligible(2.0, float('nan'), good, good))
        self.assertFalse(trainer.activation_eligible(2.0, 1.9, good, {**good, 'safe_outputs': False}))
        self.assertFalse(trainer.activation_eligible(2.0, 1.9, None, None))

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

    def test_curation_prefers_latest_explicit_corrections_over_chat(self):
        examples = [dict(self.record(' STATUS ', 'old'), source='reviewed'),
                    dict(self.record('status', 'taught'), source='teaching'),
                    dict(self.record('status', 'self-generated'), source='chat'),
                    dict(self.record('Status', 'latest correction'), source='reviewed'),
                    dict(self.record('bye', 'later'), source='chat')]
        selected, report = trainer.curate_examples(examples)
        self.assertEqual([item['preferred'] for item in selected], ['latest correction', 'later'])
        self.assertEqual(report, dict(input=5, selected=2, conflicting_or_duplicate_prompts=3,
                                      chat_selected=1, explicit_selected=1, imported_selected=0))

    def test_curation_bounds_chat_replay_without_losing_recent_corrections(self):
        examples = [dict(self.record('explicit', 'old'), source='teaching')]
        examples += [dict(self.record(f'chat {i}', 'reply'), source='chat') for i in range(100)]
        examples += [dict(self.record(f'explicit {i}', 'correct'), source='reviewed') for i in range(10)]
        examples += [dict(self.record('explicit', 'new'), source='teaching')]
        selected, report = trainer.curate_examples(examples)
        self.assertEqual(report['chat_selected'], 32)
        self.assertEqual(report['explicit_selected'], 11)
        self.assertEqual(selected[-1]['prompt'], 'chat 99')
        selected, _ = trainer.curate_examples(examples, limit=2)
        self.assertEqual([item['prompt'] for item in selected], ['explicit 9', 'explicit'])
        selected, _ = trainer.curate_examples(examples, chat_limit=0)
        self.assertTrue(all(item['source'] != 'chat' for item in selected))
        for options in (dict(limit=1), dict(chat_limit=-1)):
            with self.assertRaises(ValueError):
                trainer.curate_examples(examples, **options)

    def test_normalized_prompts_never_leak_across_holdout(self):
        examples = [self.record('HELLO there', 'hi'), self.record(' hello  THERE ', 'hey'),
                    self.record('bye', 'later')]
        train, validation = trainer.split_examples(examples)
        self.assertFalse({trainer.prompt_key(item['prompt']) for item in train}
                         & {trainer.prompt_key(item['prompt']) for item in validation})
        with self.assertRaises(ValueError):
            trainer.split_examples(examples[:2])
        with self.assertRaises(ValueError):
            self.load(examples[:2])

    def test_provenance_cannot_claim_review_without_review_flag(self):
        for change in (dict(source='unknown'), dict(source='reviewed', reviewed=False, chat_learning_consent=True)):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'invalid.jsonl'
                path.write_text(json.dumps({**self.record(), **change}) + '\n' + json.dumps(self.record('bye', 'later')))
                with self.assertRaises(ValueError):
                    trainer.load_examples(path, allow_consented_chat=True)

    def test_activation_refuses_nonfinite_or_malformed_behavior_scores(self):
        good = dict(score=0.5, safe_outputs=True)
        for score in (float('nan'), float('inf'), -1, 2, None, '0.6'):
            with self.subTest(score=score):
                self.assertFalse(trainer.activation_eligible(2.0, 1.9, good, dict(score=score, safe_outputs=True)))
        self.assertFalse(trainer.activation_eligible(2.0, 1.9, good, {}))

    def test_validate_only_reports_curation_through_cli(self):
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'chat.jsonl'
            records = [dict(self.record('status', 'Ready.'), source='teaching'),
                       dict(self.record('STATUS', 'outdated'), source='chat'), self.record('bye', 'Later.')]
            path.write_text('\n'.join(json.dumps(item) for item in records))
            result = subprocess.run([sys.executable, str(Path(trainer.__file__)), '--dataset', str(path),
                                     '--validate-only', '--automatic-evaluation'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report['examples'], 2)
            self.assertEqual(report['curation']['explicit_selected'], 2)
            self.assertEqual(report['curation']['chat_selected'], 0)
            self.assertEqual(report['train'] + report['validation'], report['examples'])

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

    def test_domain_is_declared_or_inferred_and_validated(self):
        # An export written before domains existed still takes part in domain
        # curation, because the field is inferred from the text rather than
        # required from the user.
        examples = self.load([self.record('What is the escape velocity of the Sun?', 'About 618 km/s.'),
                              self.record('hello', 'hi')])
        by_prompt = {item['prompt']: item['domain'] for item in examples}
        self.assertEqual(by_prompt['What is the escape velocity of the Sun?'], 'astronomy')
        self.assertEqual(by_prompt['hello'], 'general')

        declared = self.load([{**self.record('anything', 'fine'), 'domain': 'Astrophysics'},
                              self.record('hello', 'hi')])
        self.assertEqual(declared[0]['domain'], 'astronomy')
        with self.assertRaises(ValueError):
            self.load([{**self.record(), 'domain': 'astrology'}, self.record('bye', 'later')])
        with self.assertRaises(ValueError):
            self.load([{**self.record(), 'domain': 5}, self.record('bye', 'later')])

    def test_domain_gate_counts_distinct_specialist_prompts(self):
        examples = self.load([
            self.record('What is a pulsar?', 'A rotating neutron star.'),
            self.record('How far is a parsec?', 'About 3.26 light-years.'),
            self.record('hello', 'hi')])
        self.assertEqual(trainer.domain_prompt_count(examples, 'astronomy'), 2)
        with self.assertRaises(ValueError) as raised:
            trainer.require_domain(examples, 'astronomy')
        self.assertIn('--include-seeds', str(raised.exception))
        trainer.require_domain(examples, 'astronomy', minimum=2)
        trainer.require_domain(examples, 'general')
        with self.assertRaises(ValueError):
            trainer.require_domain(examples, 'astrology')

    def test_seed_corpus_is_reviewed_specialist_material(self):
        seeds = trainer.load_examples(trainer.SEED_DATASET)
        self.assertEqual(len(seeds), 24)
        self.assertTrue(all(seed['source'] == 'seed' and seed['reviewed'] is True for seed in seeds))
        self.assertEqual(trainer.domain_prompt_count(seeds, 'astronomy'), len(seeds))
        self.assertGreaterEqual(len(seeds), trainer.MIN_DOMAIN_PROMPTS)

    def test_seed_merge_never_overrides_a_user_correction(self):
        seeds = trainer.load_examples(trainer.SEED_DATASET)
        taught = seeds[0]['prompt']
        merged, added = trainer.merge_examples([self.record(taught, 'my own answer')], seeds)
        self.assertEqual(added, len(seeds) - 1)
        self.assertEqual(next(item for item in merged if item['prompt'] == taught)['preferred'],
                         'my own answer')
        prompts = [trainer.prompt_key(item['prompt']) for item in merged]
        self.assertEqual(len(prompts), len(set(prompts)))

    def test_curation_reserves_slots_for_the_specialist_field(self):
        # Generic corrections must not train the field out of the adapter just
        # because they are newer.
        examples = [dict(self.record(f'generic {i}', 'reply'), source='teaching') for i in range(8)]
        examples += [dict(self.record(f'What is a pulsar? {i}', 'A rotating neutron star.'), source='chat')
                     for i in range(5)]
        selected, report = trainer.curate_examples(examples, limit=8, domain='astronomy')
        self.assertLessEqual(len(selected), 8)
        self.assertGreaterEqual(report['domain_selected'], 4)
        self.assertGreater(report['domain_reserved'], 0)
        self.assertEqual(report['domain'], 'astronomy')
        order = {trainer.prompt_key(item['prompt']): index for index, item in enumerate(examples)}
        self.assertEqual(selected, sorted(selected, key=lambda item: order[trainer.prompt_key(item['prompt'])]))

    def test_activation_refuses_a_specialist_regression(self):
        general = {'score': 0.5, 'safe_outputs': True}
        before = {**general, 'domain_scores': {'astronomy': 0.6}}
        unchanged = {**general, 'domain_scores': {'astronomy': 0.6}}
        dropped = {**general, 'domain_scores': {'astronomy': 0.5}}
        self.assertTrue(trainer.activation_eligible(2.0, 1.9, before, unchanged, 'astronomy'))
        self.assertFalse(trainer.activation_eligible(2.0, 1.9, before, dropped, 'astronomy'))
        self.assertFalse(trainer.activation_eligible(2.0, 1.9, before, general, 'astronomy'))
        # Without a requested field the aggregate probe still governs, and
        # 'general' is not a field that needs its own gate.
        self.assertTrue(trainer.activation_eligible(2.0, 1.9, before, dropped))
        self.assertTrue(trainer.activation_eligible(2.0, 1.9, before, dropped, 'general'))

    def test_validate_only_merges_seed_corpus_through_cli(self):
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'chat.jsonl'
            records = [self.record('What is the escape velocity of the Sun?', 'About 618 km/s.'),
                       self.record('hello', 'hi')]
            path.write_text('\n'.join(json.dumps(item) for item in records))
            result = subprocess.run([sys.executable, str(Path(trainer.__file__)), '--dataset', str(path),
                                     '--validate-only', '--automatic-evaluation', '--domain', 'astronomy',
                                     '--include-seeds'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report['domain'], 'astronomy')
            self.assertGreater(report['seeds_added'], 0)
            self.assertGreaterEqual(report['domain_prompts']['astronomy'], trainer.MIN_DOMAIN_PROMPTS)
            self.assertGreater(report['curation']['domain_selected'], 0)

    def test_tool_seed_corpus_is_stage_scoped_and_merged_through_cli(self):
        seeds = trainer.load_examples(trainer.TOOL_SEED_DATASET, adapter_stage='tool')
        self.assertEqual(len(seeds), 8)
        self.assertTrue(all(item['adapter_stage'] == 'tool' for item in seeds))
        self.assertTrue(all(trainer.parse_tool_target(item['preferred']) for item in seeds))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'tool.jsonl'
            records = [
                {**self.record('Call first tool', '<tool_call>{"name":"alpha","arguments":{}}</tool_call>'), 'adapter_stage': 'tool'},
                {**self.record('Call second tool', '<tool_call>{"name":"beta","arguments":{}}</tool_call>'), 'adapter_stage': 'tool'},
            ]
            path.write_text('\n'.join(json.dumps(item) for item in records))
            result = __import__('subprocess').run([
                __import__('sys').executable, str(Path(trainer.__file__)), '--dataset', str(path),
                '--adapter-stage', 'tool', '--include-tool-seeds', '--automatic-evaluation', '--validate-only'],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report['adapter_stage'], 'tool')
            self.assertEqual(report['examples'], 10)
            self.assertEqual(report['seeds_added'], 8)
            wrong_stage = __import__('subprocess').run([
                __import__('sys').executable, str(Path(trainer.__file__)), '--dataset', str(path),
                '--include-tool-seeds', '--validate-only'], capture_output=True, text=True)
            self.assertNotEqual(wrong_stage.returncode, 0)
            self.assertIn('dataset must target', wrong_stage.stderr)


class ImportedDatasetTests(unittest.TestCase):
    def pair(self, prompt="What is a pulsar?", answer="A rapidly rotating neutron star whose beams sweep past Earth."):
        return {'question': prompt, 'answer': answer}

    def test_importer_maps_columns_and_records_provenance(self):
        selected, report = importer.convert(
            [self.pair(), self.pair('What is a comet?', 'A small body of ice and dust that forms a coma near the Sun.')],
            'question', 'answer', 'astronomy', 'cc-by-4.0', 'Example/astronomy-qa',
            limit=10, min_answer_chars=20)
        self.assertEqual(report['selected'], 2)
        self.assertEqual(selected[0]['source'], 'imported')
        self.assertFalse(selected[0]['reviewed'])
        self.assertEqual(selected[0]['base_model'], trainer.BASE_MODEL)
        self.assertEqual(selected[0]['domain'], 'astronomy')
        self.assertEqual(selected[0]['license'], 'cc-by-4.0')
        self.assertEqual(selected[0]['origin'], 'Example/astronomy-qa')
        self.assertEqual(selected[0]['rejected'], '')

    def test_importer_filters_rows_and_refuses_overwrite(self):
        records = [self.pair(), self.pair('again', 'A second usable answer that is long enough to pass.'),
                   {'question': 'missing answer'},
                   self.pair('short', 'Too short.'),
                   self.pair('WHAT IS A PULSAR?', 'A duplicate prompt with a different answer, so it is dropped.')]
        selected, report = importer.convert(records, 'question', 'answer', 'astronomy', 'mit', 'Example/two',
                                            min_answer_chars=20)
        self.assertEqual(report['skipped']['missing_field'], 1)
        self.assertEqual(report['skipped']['too_short'], 1)
        self.assertEqual(report['skipped']['duplicate_prompt'], 1)
        self.assertEqual([item['prompt'] for item in selected], ['What is a pulsar?', 'again'])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'imported.jsonl'
            importer.write_dataset(path, selected)
            self.assertTrue(path.read_text(encoding='utf-8').endswith('\n'))
            with self.assertRaises(ValueError):
                importer.write_dataset(path, selected)
        with self.assertRaises(ValueError):
            importer.convert(records, 'question', 'answer', 'astronomy', '', 'Example/two')

    def imported(self, prompt, answer, **changes):
        return {**dict(schema_version=1, reviewed=False, source='imported',
                       base_model=trainer.BASE_MODEL, domain='astronomy',
                       license='mit', origin='Example/three',
                       prompt=prompt, preferred=answer, rejected=''), **changes}

    def test_imported_examples_need_permission_and_cannot_claim_review(self):
        records = [self.imported('What is a pulsar?', 'A rotating neutron star.'),
                   self.imported('What is a comet?', 'An icy body that forms a coma.')]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'imported.jsonl'
            path.write_text('\n'.join(json.dumps(item) for item in records))
            with self.assertRaises(ValueError) as raised:
                trainer.load_examples(path)
            self.assertIn('--allow-imported', str(raised.exception))
            self.assertEqual(len(trainer.load_examples(path, allow_imported=True)), 2)
            records[0]['reviewed'] = True
            path.write_text('\n'.join(json.dumps(item) for item in records))
            with self.assertRaises(ValueError):
                trainer.load_examples(path, allow_imported=True)
            records[0]['reviewed'] = False
            records[0]['license'] = ''
            path.write_text('\n'.join(json.dumps(item) for item in records))
            with self.assertRaises(ValueError):
                trainer.load_examples(path, allow_imported=True)

    def test_curation_keeps_imported_material_below_a_correction(self):
        examples = [self.imported('status', 'Imported answer'),
                    {**self.imported('status', 'Taught answer'), 'source': 'teaching', 'reviewed': True}]
        selected, report = trainer.curate_examples(examples)
        self.assertEqual([item['preferred'] for item in selected], ['Taught answer'])
        self.assertEqual(report['imported_selected'], 0)


if __name__ == "__main__":
    unittest.main()
