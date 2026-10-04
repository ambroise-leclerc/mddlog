"""Mutation tests: inconsistent dossiers must fail without any C++ build."""

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

REPOSITORY = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('development_file', REPOSITORY / 'scripts/check-development-file.py')
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class DevelopmentFileTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        shutil.copytree(REPOSITORY / 'software_development_file', self.root / 'software_development_file')
        self.register = self.root / 'software_development_file/register.json'
        self.data = json.loads(self.register.read_text(encoding='utf-8'))
        # Copy only referenced files; the mutation fixture needs no modules or downloaded packages.
        referenced = set()
        for row in self.data['controls']:
            referenced.update(row['design'] + row['implementation'])
        for row in self.data['verifications']:
            referenced.update((row['test'], row['evidence']))
        for row in self.data['dependencies']:
            referenced.add(row['provenance'])
        referenced.update(('CMakePresets.json', 'CONTRIBUTING.md', 'LICENSE', '.github/CODEOWNERS',
                           'docs/adr/README.md', 'docs/release-process.md'))
        for name in referenced:
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPOSITORY / name, destination)

    def errors(self):
        self.register.write_text(json.dumps(self.data), encoding='utf-8')
        return CHECKER.check(self.root)

    def assertRejected(self, text):
        self.assertTrue(any(text in error for error in self.errors()), text)

    def test_valid_initial_dossier(self):
        self.assertEqual(self.errors(), [])

    def test_duplicate_identifier(self):
        self.data['risks'].append(self.data['risks'][0].copy())
        self.assertRejected('duplicate id')

    def test_reference_must_resolve_to_correct_type(self):
        self.data['requirements'][0]['risks'] = ['CTRL-001']
        self.assertRejected('unresolved RISK reference')

    def test_missing_implementation(self):
        (self.root / self.data['controls'][1]['implementation'][0]).unlink()
        self.assertRejected('missing or out-of-repository file')

    def test_scenario_must_exist(self):
        self.data['verifications'][1]['scenario'] = 'invented-test-name'
        self.assertRejected('scenario absent')

    def test_planned_requirement_needs_open_gap(self):
        self.data['gaps'][5]['status'] = 'closed'
        self.assertRejected('planned requirement needs an open gap')

    def test_verification_must_cover_requirement_control(self):
        self.data['requirements'][0]['verifications'] = ['VER-002']
        self.assertRejected('covers none of its controls')

    def test_review_cannot_be_accepted_without_decision(self):
        self.data['review']['status'] = 'accepted'
        errors = self.errors()
        self.assertTrue(any('reviewed commit' in error for error in errors))
        self.assertTrue(any('ISO date' in error for error in errors))
        self.assertTrue(any('rationale' in error for error in errors))

    def test_unknown_register_format(self):
        self.data['formatVersion'] = 2
        self.assertRejected('unsupported formatVersion')

    def test_malformed_list_and_status_report_errors(self):
        self.data['requirements'][0]['controls'] = 123
        self.data['requirements'][0]['status'] = []
        self.assertRejected('expected list')

    def test_duplicate_json_key(self):
        self.register.write_text('{"formatVersion":1,"formatVersion":1}', encoding='utf-8')
        self.assertTrue(any('duplicate JSON key' in error for error in CHECKER.check(self.root)))

    def test_broken_local_link(self):
        with (self.root / 'software_development_file/README.md').open('a', encoding='utf-8') as doc:
            doc.write('\n[Missing](regulatory/Missing.md)\n')
        self.assertRejected('regulatory/Missing.md')

    def test_outside_repository_reference(self):
        self.data['controls'][0]['implementation'] = ['../../outside.cppm']
        self.assertRejected('out-of-repository')

    def test_unresolved_document_identifier(self):
        with (self.root / 'software_development_file/regulatory/Plans.md').open('a', encoding='utf-8') as doc:
            doc.write('\nREQ-999\n')
        self.assertRejected('unresolved id REQ-999')

    def test_missing_required_document(self):
        (self.root / 'software_development_file/templates/IEC_62304/SAD.md').unlink()
        self.assertRejected('document inventory')


if __name__ == '__main__':
    unittest.main()
