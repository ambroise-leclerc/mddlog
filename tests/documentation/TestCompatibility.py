"""Inventory, format, history and snippet controls of scripts/check-compatibility.py (#121)."""
import importlib.util
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('compatibility', ROOT / 'scripts/check-compatibility.py')
COMPATIBILITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPATIBILITY)

# The repository subset the checker reads, copied so negative controls never touch the checkout.
SUBSET = ('CMakeLists.txt', 'cmake/mddlogConfig.cmake.in', 'cmake/MddlogBuildInfo.cmake', 'include',
          'tools/AuditTool.cpp', 'scripts/package-source.py', 'docs/api/public-surface.json',
          'docs/adr/ADR-007-compatibility-and-distribution.md', 'docs/migration',
          'tests/consumer/package/MigrationExamples.cpp', 'tests/archives/audit-export')


def git(root, *arguments):
    subprocess.run(['git', '-C', str(root), *arguments], check=True, capture_output=True,
                   env={'GIT_AUTHOR_NAME': 'test', 'GIT_AUTHOR_EMAIL': 'test@example.invalid',
                        'GIT_COMMITTER_NAME': 'test', 'GIT_COMMITTER_EMAIL': 'test@example.invalid',
                        'GIT_CONFIG_GLOBAL': '/dev/null', 'GIT_CONFIG_SYSTEM': '/dev/null',
                        'PATH': '/usr/bin:/bin:/usr/local/bin'})


class CompatibilityTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        for item in SUBSET:
            source, target = ROOT / item, self.root / item
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)

    def tearDown(self):
        self.directory.cleanup()

    def edit(self, path, old, new):
        file = self.root / path
        text = file.read_text(encoding='utf-8')
        self.assertIn(old, text, f'fixture drift in {path}')
        file.write_text(text.replace(old, new, 1), encoding='utf-8')

    def surface(self, mutate):
        file = self.root / 'docs/api/public-surface.json'
        data = json.loads(file.read_text(encoding='utf-8'))
        mutate(data)
        file.write_text(json.dumps(data, indent=2), encoding='utf-8')

    def assertRefused(self, fragment, use_git=False):
        errors = COMPATIBILITY.check(self.root, use_git)
        self.assertTrue(any(fragment in error for error in errors), errors)

    def test_repository_is_consistent(self):
        self.assertEqual(COMPATIBILITY.check(ROOT), [])
        self.assertEqual(COMPATIBILITY.check(self.root), [])

    def test_unregistered_or_uninventoried_module_is_refused(self):
        (self.root / 'include/mddlog/adapter/Extra.cppm').write_text('export module mddlog.adapter.extra;\n')
        self.assertRefused('module interface not registered')
        self.edit('CMakeLists.txt', 'include/mddlog/Log.cppm\n', 'include/mddlog/Log.cppm\n        include/mddlog/adapter/Extra.cppm\n')
        self.assertRefused('module mddlog.adapter.extra: not inventoried')

    def test_component_move_is_refused(self):
        self.surface(lambda data: data['modules']['mddlog.adapter.filestoragemedium'].update(component='full'))
        self.assertRefused('component is full, CMake says file_storage')

    def test_umbrella_change_is_refused(self):
        self.edit('include/mddlog/mddlog.cppm', 'using adapter::AuditChain;\n', '')
        self.assertRefused('umbrella name AuditChain: inventoried but not exported')
        self.edit('include/mddlog/mddlog.cppm', 'using core::WriteResult;\n', 'using core::WriteResult;\nusing core::Unlisted;\n')
        self.assertRefused('umbrella name Unlisted: exported but not inventoried')

    def test_silent_format_version_change_is_refused(self):
        self.edit('include/mddlog/adapter/AuditLayout.cppm', 'storageLayoutVersion = 1;', 'storageLayoutVersion = 2;')
        self.assertRefused('format segment-layout')

    def test_installed_warnings_and_missing_component_are_refused(self):
        self.edit('CMakeLists.txt', 'set(_mddlog_installed_targets mddlog mddlog-core mddlog_options)',
                  'set(_mddlog_installed_targets mddlog mddlog-core mddlog_options mddlog_warnings)')
        self.assertRefused('mddlog_warnings must stay build-only')
        self.edit('cmake/mddlogConfig.cmake.in', 'set(mddlog_audit_tool_FOUND', 'set(mddlog_audit_tool_PRESENT')
        self.assertRefused('component audit_tool never reports FOUND')

    def test_migration_snippet_drift_is_refused(self):
        self.edit('tests/consumer/package/MigrationExamples.cpp', 'logger.info("Primed");', 'logger.info("Ready");')
        self.assertRefused('snippet diagnostic-after differs')

    def test_duplicate_json_key_is_refused(self):
        file = self.root / 'docs/api/public-surface.json'
        file.write_text(file.read_text(encoding='utf-8').replace('{', '{"status": "candidate", ', 1), encoding='utf-8')
        self.assertRefused('duplicate JSON key')

    @unittest.skipUnless(shutil.which('git'), 'git is required')
    def test_release_history_controls(self):
        git(self.root, 'init', '-q')
        git(self.root, 'add', '-A')
        git(self.root, 'commit', '-q', '-m', 'fixture')
        # A synthetic release: every current name is "released" in 0.2.0 by this tag.
        git(self.root, 'tag', 'v0.2.0')
        self.assertRefused('since 0.1.0, first released in 0.2.0', use_git=True)
        self.surface(lambda data: [entry.update(since='0.2.0')
                                   for section in (data['modules'], data['umbrella']['names'])
                                   for entry in section.values()])
        self.assertEqual(COMPATIBILITY.check(self.root, True), [])

        # A removal after release needs a removed[] record; an archive fixture is required.
        self.edit('include/mddlog/mddlog.cppm', 'using adapter::AuditChain;\n', '')
        self.surface(lambda data: data['umbrella']['names'].pop('AuditChain'))
        self.assertRefused('umbrella name AuditChain: released in 0.2.0, now absent', use_git=True)
        self.surface(lambda data: data['removed'].append(
            {'name': 'AuditChain', 'kind': 'umbrella name', 'deprecatedIn': '0.3.0', 'removedIn': '1.0.0',
             'migration': 'docs/migration/v0.2-to-1.0.md'}))
        self.assertEqual(COMPATIBILITY.check(self.root, True), [])
        (self.root / 'tests/archives/audit-export/v0.2.0.json').unlink()
        self.assertRefused('release v0.2.0: no audit archive fixture', use_git=True)

    def test_snippet_regions_require_matching_markers(self):
        regions = COMPATIBILITY.snippet_regions('// [migration:a]\nx\n// [/migration:b]\n// [migration:c]\ny\n// [/migration:c]\n')
        self.assertEqual(regions, {'c': 'y'})


if __name__ == '__main__':
    unittest.main()
