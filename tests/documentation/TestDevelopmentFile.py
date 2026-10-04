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
        # Start mutation tests from a proposed review even after the real baseline is accepted.
        self.data['review'].update(status='proposed', reviewedCommit=None, date=None,
                                   decision=None, acceptedGaps={})
        for row in self.data['gaps']:
            if row['id'] in self.data['review']['reservations']:
                row['status'] = 'open'
        # Copy only referenced files; the mutation fixture needs no modules or downloaded packages.
        referenced = set()
        for row in self.data['controls']:
            referenced.update(row['design'] + row['implementation'])
        for row in self.data['verifications']:
            referenced.update((row['test'], row['evidence']))
        for row in self.data['dependencies']:
            referenced.add(row['provenance'])
        referenced.update(('CMakePresets.json', 'CONTRIBUTING.md', 'LICENSE', '.github/CODEOWNERS',
                           'docs/adr/README.md', 'docs/release-process.md',
                           'docs/adr/ADR-005-contextual-logging-api.md', 'docs/contextual-api-study.md'))
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
        self.data['formatVersion'] = 3
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


    def accept_review(self):
        self.data['review'].update(status='accepted', reviewedCommit='a' * 40,
                                   date='2026-10-04', decision='Base reviewed with stated limitations.')
        reservations = set(self.data['review']['reservations'])
        for row in self.data['gaps']:
            if row['id'] in reservations:
                row['status'] = 'closed'
        self.data['review']['acceptedGaps'] = {
            ref: 'Qualification deferred to the referenced epic; no deployment claim accepted.'
            for row in self.data['requirements'] if row['status'] == 'implemented'
            for ref in row['gaps'] if ref not in reservations
        }

    def test_accepted_review_with_closed_reservations_and_explicit_gap_decisions(self):
        self.accept_review()
        self.assertEqual(self.errors(), [])

    def test_open_reservation_blocks_acceptance_even_with_rationale(self):
        self.accept_review()
        self.data['gaps'][0]['status'] = 'open'
        self.data['review']['acceptedGaps']['GAP-001'] = 'Deferred.'
        self.assertRejected('reservation GAP-001 must be closed')

    def test_implemented_requirement_gap_needs_explicit_decision(self):
        self.accept_review()
        self.data['review']['acceptedGaps'].pop('GAP-016')
        self.assertRejected('open gap GAP-016 needs explicit acceptance rationale')

    def test_empty_acceptance_rationale_rejected(self):
        self.accept_review()
        self.data['review']['acceptedGaps']['GAP-016'] = '  '
        self.assertRejected('needs an explicit rationale')

    def test_proposed_review_cannot_accept_gaps(self):
        self.data['review']['acceptedGaps']['GAP-016'] = 'Deferred.'
        self.assertRejected('acceptedGaps requires an accepted review')

    def test_implemented_requirement_control_status(self):
        self.data['controls'][0]['status'] = 'planned'
        self.assertRejected('incompatible controls status for CTRL-001')

    def test_implemented_requirement_verification_status(self):
        self.data['verifications'][0]['status'] = 'planned'
        self.assertRejected('incompatible verifications status for VER-001')

    def test_scenario_in_comment_or_unrelated_literal_is_insufficient(self):
        test = self.root / self.data['verifications'][1]['test']
        for content in ('// speclab::Test("invented-test-name")',
                        '/* speclab::Test("invented-test-name") */',
                        'const char* id = "invented-test-name";',
                        'auto text = R"example(speclab::Test("invented-test-name"))example";'):
            with self.subTest(content=content):
                test.write_text(content, encoding='utf-8')
                self.data['verifications'][1]['scenario'] = 'invented-test-name'
                self.assertRejected('scenario absent')

    def test_template_scenario_call_is_recognized(self):
        self.assertEqual(CHECKER.cpp_scenarios(
            'speclab::Test<State<Nested<int>>>("scenario-id");'), {'scenario-id'})

    def test_markdown_paths_with_parentheses_titles_and_angle_brackets(self):
        dossier = self.root / 'software_development_file'
        (dossier / 'Note (review).md').write_text('# Révision\n', encoding='utf-8')
        (dossier / 'Note(review).md').write_text('# Révision\n', encoding='utf-8')
        with (dossier / 'README.md').open('a', encoding='utf-8') as doc:
            doc.write('\n[Note](Note(review).md "Title (review)")\n'
                      "[Note](<Note (review).md> 'Title')\n"
                      '[Note](Note(review).md (Title))\n')
        self.assertEqual(self.errors(), [])

    def test_same_document_and_cross_document_anchors(self):
        dossier = self.root / 'software_development_file'
        (dossier / 'Note.md').write_text('# Révision *locale*\n# Révision *locale*\n', encoding='utf-8')
        with (dossier / 'README.md').open('a', encoding='utf-8') as doc:
            doc.write('\n[Here](#index)\n[There](Note.md#révision-locale-1 "Title")\n')
        self.assertEqual(self.errors(), [])

    def test_missing_anchor_is_rejected(self):
        with (self.root / 'software_development_file/README.md').open('a', encoding='utf-8') as doc:
            doc.write('\n[Missing](#invented-section)\n')
        self.assertRejected('missing Markdown anchor')

    def test_heading_in_code_fence_is_not_an_anchor(self):
        with (self.root / 'software_development_file/README.md').open('a', encoding='utf-8') as doc:
            doc.write('\n~~~md\n# Invented section\n~~~\n[Missing](#invented-section)\n')
        self.assertRejected('missing Markdown anchor')

    def test_malformed_inline_link_is_rejected(self):
        with (self.root / 'software_development_file/README.md').open('a', encoding='utf-8') as doc:
            doc.write('\n[Note](README.md "unterminated)\n')
        self.assertRejected('unterminated link title')

    def test_reference_links_are_explicitly_unsupported(self):
        with (self.root / 'software_development_file/README.md').open('a', encoding='utf-8') as doc:
            doc.write('\n[Note][reference]\n[reference]: missing.md\n')
        self.assertRejected('reference-style links are unsupported')


if __name__ == '__main__':
    unittest.main()
