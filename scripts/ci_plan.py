#!/usr/bin/env python3
"""Select CI jobs conservatively and reject incomplete required checks."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


JOB_FLAGS = {
    'clang-format': 'format',
    'cppcheck': 'analysis',
    'unit-tests': 'tests',
    'select_runner': 'firmware',
    'build-default': 'firmware',
    'build-simulator': 'simulator',
}
DOC_SUFFIXES = {'.md', '.rst', '.txt', '.png', '.jpg', '.jpeg', '.svg', '.gif', '.webp', '.pdf'}


def select_jobs(paths, force_all=False):
    plan = dict.fromkeys(set(JOB_FLAGS.values()), force_all)
    for name in paths:
        path = Path(name)
        if ((name.startswith('docs/') and path.suffix.lower() in DOC_SUFFIXES)
                or ('/' not in name and path.suffix.lower() == '.md')
                or name in {'LICENSE', 'LICENSE.txt', 'LICENSE.md'}):
            continue
        plan['tests'] = True
        if name.startswith(('test/', 'scripts/tests/')):
            if path.suffix.lower() in {'.c', '.cpp', '.h', '.hpp'}:
                plan['format'] = True
            continue
        # Unknown paths, toolchains, shared code and CI changes get full coverage.
        # Add narrow exclusions only when their independent verification is known.
        plan = dict.fromkeys(plan, True)
    return plan


def changed_paths(base, head, merge_base=False):
    separator = '...' if merge_base else '..'
    revision = f'{base}{separator}{head}' if head else base
    raw = subprocess.check_output([
        'git', 'diff', '--name-only', '--no-renames', '-z', revision, '--',
    ])
    if head is None:
        raw += subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'])
    return [os.fsdecode(name).replace('\\', '/') for name in raw.split(b'\0') if name]


def event_plan(event, event_name, head):
    if event_name == 'workflow_dispatch':
        return select_jobs([], force_all=True)
    base = event.get('before') if event_name == 'push' else event.get('pull_request', {}).get('base', {}).get('sha')
    if not base or set(base) == {'0'}:
        return select_jobs([], force_all=True)
    try:
        paths = changed_paths(base, head, merge_base=event_name == 'pull_request')
    except subprocess.CalledProcessError:
        # Deleted/force-pushed bases can be absent even in a full checkout.
        print('Base unavailable; selecting all checks.', file=sys.stderr)
        return select_jobs([], force_all=True)
    return select_jobs(paths)


def failed_jobs(needs):
    changes = needs.get('changes', {})
    if changes.get('result') != 'success':
        return ['changes']
    plan = changes.get('outputs', {})
    missing_flags = set(JOB_FLAGS.values()) - plan.keys()
    if missing_flags or any(plan[flag] not in {'true', 'false'} for flag in set(JOB_FLAGS.values())):
        return ['changes (invalid plan)']
    failed = []
    for job, flag in JOB_FLAGS.items():
        result = needs.get(job, {}).get('result', 'missing')
        required = plan[flag] == 'true'
        if result not in {'success', 'skipped'} or (required and result != 'success'):
            failed.append(job)
    return failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    plan_parser = commands.add_parser('plan', help='Print JSON and optional GitHub job outputs')
    plan_parser.add_argument('--base', help='Compare local changes against this Git ref')
    plan_parser.add_argument('--head', help='Compare committed refs; otherwise include the local working tree')
    plan_parser.add_argument('--all', action='store_true', help='Select all checks')
    commands.add_parser('result', help='Check NEEDS_JSON and write the job summary')
    args = parser.parse_args()
    if args.command == 'plan':
        if args.all:
            plan = select_jobs([], force_all=True)
        elif args.base:
            plan = select_jobs(changed_paths(args.base, args.head))
        elif os.environ.get('GITHUB_EVENT_PATH'):
            event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8'))
            plan = event_plan(event, os.environ['GITHUB_EVENT_NAME'], os.environ['GITHUB_SHA'])
        else:
            plan = select_jobs([], force_all=True)
        print(json.dumps(plan, sort_keys=True))
        if output := os.environ.get('GITHUB_OUTPUT'):
            with open(output, 'a', encoding='utf-8') as stream:
                for key, enabled in sorted(plan.items()):
                    stream.write(f'{key}={str(enabled).lower()}\n')
        return 0
    needs = json.loads(os.environ['NEEDS_JSON'])
    failures = failed_jobs(needs)
    lines = ['## CI checks', '', '| Check | Result |', '|---|---|']
    for job in ['changes', *JOB_FLAGS]:
        lines.append(f"| {job} | {needs.get(job, {}).get('result', 'missing')} |")
    lines.extend(['', 'Failed checks: ' + ', '.join(failures) if failures else 'All selected checks passed.'])
    summary = '\n'.join(lines) + '\n'
    print(summary)
    if output := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(output, 'a', encoding='utf-8') as stream:
            stream.write(summary)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
