#!/usr/bin/env python3
"""Promote an exact successful CI artifact; never compile or fall back to another revision."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

from ci_firmware_package import (
    TARGET, context, expected_identity, package_names, provenance_name, seal, verify_seal,
)

WORKFLOW = '.github/workflows/ci.yml'
MAX_ARCHIVE = 32 * 1024 * 1024
MAX_JSON = 2 * 1024 * 1024


def artifact_name(attempt):
    return f'firmware-nightly-{TARGET}-ci-{attempt}'


def validate_run(run, repository, ref, revision):
    expected = context(repository, ref, run['id'], run['run_attempt'])
    if (not ref.startswith(('refs/heads/', 'refs/tags/'))
            or run.get('status') != 'completed' or run.get('conclusion') != 'success'
            or run.get('event') not in {'push', 'workflow_dispatch'} or run.get('path') != WORKFLOW
            or run.get('head_sha') != revision or run.get('head_branch') != ref.split('/', 2)[2]
            or run.get('repository', {}).get('full_name') != repository
            or run.get('head_repository', {}).get('full_name') != repository
            or type(run.get('repository', {}).get('id')) is not int or run['repository']['id'] <= 0
            or run.get('repository', {}).get('id') != run.get('head_repository', {}).get('id')):
        raise ValueError('CI must be successful, from this repository/ref/workflow and exact release SHA')
    return expected


def validate_artifact(artifact, run):
    source = artifact.get('workflow_run', {})
    if (type(artifact.get('id')) is not int or artifact['id'] <= 0
            or artifact.get('name') != artifact_name(run['run_attempt']) or artifact.get('expired') is not False
            or type(artifact.get('size_in_bytes')) is not int or not 0 < artifact['size_in_bytes'] <= MAX_ARCHIVE
            or not isinstance(artifact.get('digest'), str)
            or not re.fullmatch(r'sha256:[0-9a-f]{64}', artifact['digest'])
            or source.get('id') != run['id'] or source.get('head_sha') != run['head_sha']
            or source.get('head_branch') != run['head_branch']
            or source.get('repository_id') != run['repository']['id']
            or source.get('head_repository_id') != run['repository']['id']):
        raise ValueError('CI artifact is missing, expired, ambiguous or has invalid provenance/digest')
    expires = datetime.fromisoformat(artifact['expires_at'].replace('Z', '+00:00'))
    if expires.tzinfo is None or expires <= datetime.now(timezone.utc):
        raise ValueError('CI artifact expired; rerun the complete CI workflow')


def select_source(api, repository, ref, revision, run_id='auto'):
    base = f'/repos/{repository}/actions'
    if run_id == 'auto':
        query = urllib.parse.urlencode({'head_sha': revision, 'branch': ref.split('/', 2)[2],
                                        'status': 'success', 'per_page': 100})
        candidates = []
        for page in range(1, 11):
            runs = api.json(f'{base}/workflows/ci.yml/runs?{query}&page={page}')['workflow_runs']
            for run in runs:
                try:
                    validate_run(run, repository, ref, revision)
                except (ValueError, KeyError):
                    continue
                candidates.append(run)
            if len(runs) < 100:
                break
        if not candidates:
            raise ValueError('No successful exact-SHA CI run; run CI before releasing')
        run_id = str(max(candidates, key=lambda run: run['id'])['id'])
    if not re.fullmatch(r'[1-9][0-9]*', run_id):
        raise ValueError('ci_run_id must be auto or a positive numeric CI run ID')
    run = api.json(f'{base}/runs/{run_id}')
    validate_run(run, repository, ref, revision)
    matches = []
    for page in range(1, 11):
        artifacts = api.json(f'{base}/runs/{run_id}/artifacts?per_page=100&page={page}')['artifacts']
        matches.extend(item for item in artifacts if item.get('name') == artifact_name(run['run_attempt']))
        if len(artifacts) < 100:
            break
    if len(matches) != 1:
        raise ValueError('Expected one complete package from this CI attempt; rerun all CI jobs')
    validate_artifact(matches[0], run)
    return run, matches[0]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


class GitHub:
    def __init__(self, token):
        if not token:
            raise ValueError('GH_TOKEN with actions:read is required')
        self.headers = {'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json',
                        'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'crossmux-ci-promotion'}
        self.opener = urllib.request.build_opener(NoRedirect())

    def json(self, path):
        request = urllib.request.Request('https://api.github.com' + path, headers=self.headers)
        with self.opener.open(request, timeout=30) as response:
            data = response.read(MAX_JSON + 1)
        if len(data) > MAX_JSON:
            raise ValueError('GitHub metadata response exceeds bound')
        return json.loads(data)

    def download(self, repository, artifact, destination):
        request = urllib.request.Request(
            f"https://api.github.com/repos/{repository}/actions/artifacts/{artifact['id']}/zip", headers=self.headers)
        try:
            self.opener.open(request, timeout=30)
        except urllib.error.HTTPError as error:
            if error.code != 302:
                raise ValueError(f'GitHub artifact download returned HTTP {error.code}') from None
            location = error.headers.get('Location', '')
        else:
            raise ValueError('Expected GitHub artifact redirect')
        parsed = urllib.parse.urlparse(location)
        if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password:
            raise ValueError('Invalid artifact download redirect')
        digest = hashlib.sha256()
        size = 0
        # The API authenticates the redirect. Never forward the token to blob storage,
        # and never follow a further redirect or print its signed query string.
        with self.opener.open(urllib.request.Request(location), timeout=60) as response, destination.open('wb') as out:
            while chunk := response.read(1024 * 1024):
                size += len(chunk)
                if size > MAX_ARCHIVE:
                    raise ValueError('CI artifact archive exceeds bound')
                out.write(chunk)
                digest.update(chunk)
        if size != artifact['size_in_bytes'] or 'sha256:' + digest.hexdigest() != artifact['digest']:
            raise ValueError('CI archive size or GitHub SHA-256 digest mismatch')


def extract_package(archive, destination):
    expected = package_names() | {provenance_name()}
    with zipfile.ZipFile(archive) as bundle:
        files = bundle.infolist()
        if len(files) != len(expected) or {entry.filename for entry in files} != expected:
            raise ValueError('CI ZIP contains missing, duplicate, nested or unexpected files')
        if sum(entry.file_size for entry in files) > MAX_ARCHIVE:
            raise ValueError('CI package expands beyond bound')
        for entry in files:
            if entry.is_dir() or stat.S_ISLNK(entry.external_attr >> 16) or entry.flag_bits & 1:
                raise ValueError('CI ZIP contains a directory, symlink or encrypted entry')
        destination.mkdir()
        for entry in files:
            with bundle.open(entry) as source, (destination / entry.filename).open('xb') as out:
                shutil.copyfileobj(source, out)


def promote(api, root, repository, ref, output, run_id='auto'):
    revision, _sdk, _version = expected_identity(root)
    context(repository, ref, 1, 1)
    run, artifact = select_source(api, repository, ref, revision, run_id)
    with tempfile.TemporaryDirectory(prefix='crossmux-ci-promote-') as directory:
        work = Path(directory)
        archive = work / 'package.zip'
        api.download(repository, artifact, archive)
        package = work / 'package'
        extract_package(archive, package)
        record = verify_seal(root, package, repository, ref, run['id'], run['run_attempt'])
        # A rerun or deletion during download must not silently change our source.
        current = api.json(f"/repos/{repository}/actions/runs/{run['id']}")
        validate_run(current, repository, ref, revision)
        current_artifact = api.json(f"/repos/{repository}/actions/artifacts/{artifact['id']}")
        validate_artifact(current_artifact, current)
        if current['run_attempt'] != run['run_attempt'] or current_artifact != artifact:
            raise ValueError('CI run/artifact changed during promotion')
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(package, output)  # Fresh destination only; never mix old release files.
    return {'runId': run['id'], 'runAttempt': run['run_attempt'], 'artifactId': artifact['id'],
            'artifactDigest': artifact['digest'], 'version': record['version'], 'crossmuxSha': revision,
            'firmwareFiles': record['files']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['seal', 'promote'])
    parser.add_argument('--package', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY'))
    parser.add_argument('--ref', default=os.environ.get('GITHUB_REF'))
    parser.add_argument('--run-id')
    parser.add_argument('--run-attempt', type=int, default=os.environ.get('GITHUB_RUN_ATTEMPT'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.command == 'seal':
        if not args.package:
            parser.error('--package is required for seal')
        seal(root, args.package, args.repository, args.ref,
             int(args.run_id or os.environ['GITHUB_RUN_ID']), args.run_attempt)
        print('Sealed complete CI package:', args.package)
    else:
        if not args.output:
            parser.error('--output is required for promote')
        result = promote(GitHub(os.environ.get('GH_TOKEN')), root, args.repository, args.ref,
                         args.output, args.run_id or 'auto')
        print(json.dumps(result, indent=2))
        if path := os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(path, 'a', encoding='utf-8') as summary:
                summary.write(f"## Reused verified CI firmware\n\nCI run: `{result['runId']}` "
                              f"attempt `{result['runAttempt']}`; artifact `{result['artifactId']}`\n\n"
                              f"SHA: `{result['crossmuxSha']}`\n\nArchive: `{result['artifactDigest']}`\n")


if __name__ == '__main__':
    main()
