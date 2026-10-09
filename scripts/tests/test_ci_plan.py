"""Regression checks for CI selection, skipped jobs and failure propagation."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ci_plan


class CiPlanTest(unittest.TestCase):
    def test_documentation_skips_builds(self):
        for paths in [[], ['README.md', 'AGENTS.md'], ['docs/guide.md', 'docs/images/menu.png']]:
            with self.subTest(paths=paths):
                self.assertFalse(any(ci_plan.select_jobs(paths).values()))

    def test_shared_sources_and_unknown_paths_require_both_builds(self):
        for path in ['src/main.cpp', 'lib/hal/HalGPIO.h', 'freeink-sdk', 'platformio.ini',
                     '.github/workflows/ci.yml', 'scripts/package_nightly_target.py',
                     'docs/check.py', 'new-build-system/config', 'src/data.md']:
            with self.subTest(path=path):
                self.assertTrue(all(ci_plan.select_jobs([path]).values()))

    def test_tests_only_keep_host_verification(self):
        plan = ci_plan.select_jobs(['scripts/tests/test_nightly_release.py'])
        self.assertTrue(plan['tests'])
        self.assertFalse(plan['firmware'])
        self.assertFalse(plan['simulator'])
        self.assertFalse(plan['analysis'])
        self.assertFalse(plan['format'])
        self.assertTrue(ci_plan.select_jobs(['test/reader/ReaderTest.cpp'])['format'])

    def test_manual_runs_always_select_everything(self):
        self.assertTrue(all(ci_plan.event_plan({}, 'workflow_dispatch', 'head').values()))

    def test_missing_or_zero_base_selects_everything(self):
        for event in [{}, {'before': '0' * 40}]:
            self.assertTrue(all(ci_plan.event_plan(event, 'push', 'head').values()))
        with patch.object(ci_plan, 'changed_paths', side_effect=subprocess.CalledProcessError(128, 'git')):
            self.assertTrue(all(ci_plan.event_plan({'before': 'missing'}, 'push', 'head').values()))

    def test_push_and_pr_use_correct_diff_bases(self):
        for name, event, merge_base in [
            ('push', {'before': 'before'}, False),
            ('pull_request', {'pull_request': {'base': {'sha': 'before'}}}, True),
        ]:
            with self.subTest(event=name), patch.object(ci_plan, 'changed_paths', return_value=['README.md']) as diff:
                self.assertFalse(any(ci_plan.event_plan(event, name, 'head').values()))
                diff.assert_called_once_with('before', 'head', merge_base=merge_base)

    @staticmethod
    def needs(paths):
        plan = ci_plan.select_jobs(paths)
        return {
            'changes': {'result': 'success', 'outputs': {key: str(value).lower() for key, value in plan.items()}},
            **{job: {'result': 'success' if plan[flag] else 'skipped'} for job, flag in ci_plan.JOB_FLAGS.items()},
        }

    def test_documentation_and_successful_code_pass(self):
        self.assertEqual(ci_plan.failed_jobs(self.needs(['README.md'])), [])
        self.assertEqual(ci_plan.failed_jobs(self.needs(['src/main.cpp'])), [])

    def test_failed_cancelled_missing_and_skipped_required_jobs_fail(self):
        for job in ci_plan.JOB_FLAGS:
            for result in ['failure', 'cancelled', 'missing', 'skipped']:
                with self.subTest(job=job, result=result):
                    needs = self.needs(['src/main.cpp'])
                    needs[job] = {'result': result}
                    self.assertIn(job, ci_plan.failed_jobs(needs))

    def test_failed_selection_and_invalid_outputs_fail_closed(self):
        for changes in [{}, {'result': 'failure'}, {'result': 'success', 'outputs': {}},
                        {'result': 'success', 'outputs': dict.fromkeys(ci_plan.JOB_FLAGS.values(), 'unexpected')}]:
            with self.subTest(changes=changes):
                needs = self.needs(['README.md'])
                needs['changes'] = changes
                self.assertTrue(ci_plan.failed_jobs(needs))

    def test_cli_result_exit_status_and_summary(self):
        for result, expected in [('success', 0), ('failure', 1), ('skipped', 1)]:
            with self.subTest(result=result), tempfile.TemporaryDirectory() as directory:
                needs = self.needs(['src/main.cpp'])
                needs['build-simulator']['result'] = result
                summary = Path(directory) / 'summary.md'
                env = {**os.environ, 'NEEDS_JSON': json.dumps(needs), 'GITHUB_STEP_SUMMARY': str(summary)}
                process = subprocess.run([sys.executable, ci_plan.__file__, 'result'], env=env,
                                         capture_output=True, text=True, encoding='utf-8')
                self.assertEqual(process.returncode, expected, process.stderr)
                self.assertIn(f'| build-simulator | {result} |', summary.read_text(encoding='utf-8'))

    def test_git_diff_detects_doc_renamed_into_source_and_untracked_code(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(['git', *args], cwd=root)
            git('init', '-q')
            git('config', 'user.name', 'CI fixture')
            git('config', 'user.email', 'ci@example.invalid')
            (root / 'docs').mkdir()
            (root / 'docs/example.md').write_text('unchanged payload\n', encoding='utf-8')
            git('add', '.')
            git('commit', '-qm', 'fixture')
            base = git('rev-parse', 'HEAD').decode().strip()
            (root / 'src').mkdir()
            (root / 'docs/example.md').rename(root / 'src/example.cpp')
            previous = Path.cwd()
            try:
                os.chdir(root)
                paths = ci_plan.changed_paths(base, None)
            finally:
                os.chdir(previous)
            self.assertIn('docs/example.md', paths)
            self.assertIn('src/example.cpp', paths)
            self.assertTrue(all(ci_plan.select_jobs(paths).values()))


if __name__ == '__main__':
    unittest.main()
