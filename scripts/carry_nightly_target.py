#!/usr/bin/env python3
"""Carry a verified, unchanged target into a branch-only Nightly release."""

import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import urljoin

from build_nightly_index import valid_manifest
from nightly_targets import FLAVOR_TOKENS, TARGETS, manifest_name
from verify_nightly_release import expected_assets, fetch_bytes, validate_url


def carry_target(previous, repository, target_id, output, fetch=fetch_bytes):
    if previous.get('schemaVersion') != 1 or previous.get('channel') != 'nightly':
        raise ValueError('invalid previous Nightly index')
    build_id = previous.get('buildId')
    if not isinstance(build_id, str) or not build_id.startswith('nightly-build-'):
        raise ValueError('invalid previous build ID')
    entry = previous.get('targets', {}).get(target_id)
    target = TARGETS[target_id]
    if not isinstance(entry, dict) or entry.get('targetId') != target_id or any(
        entry.get(key) != target[key]
        for key in ('models', 'deviceSlug', 'boardTag', 'supportedChannels')
    ):
        raise ValueError('invalid carried target')
    variants = entry.get('variants')
    if not isinstance(variants, dict) or set(variants) != set(FLAVOR_TOKENS):
        raise ValueError('incomplete carried variants')

    index_url = f'https://github.com/{repository}/releases/download/nightly/release-index.json'
    source_prefix = f'https://github.com/{repository}/releases/download/{build_id}/'
    files = {}
    manifests = []
    for flavor in FLAVOR_TOKENS:
        pointer = variants[flavor]
        name = manifest_name(target_id, flavor)
        url = pointer.get('manifestUrl') if isinstance(pointer, dict) else None
        if url != source_prefix + name:
            raise ValueError('carried manifest is outside the previous immutable build')
        validate_url(url, index_url, 'nightly')
        raw = fetch(url)
        manifest = json.loads(raw)
        if not valid_manifest(manifest, target_id, flavor, 'nightly') or any(
            manifest.get(key) != pointer.get(key)
            for key in ('version', 'crossmuxSha', 'sdkSha')
        ):
            raise ValueError('carried manifest does not match previous index')
        files[name] = raw
        manifests.append(manifest)
    if any(
        {key: value for key, value in manifest.items() if key != 'flavor'}
        != {key: value for key, value in manifests[0].items() if key != 'flavor'}
        for manifest in manifests[1:]
    ):
        raise ValueError('carried compatibility manifests differ')
    assets = manifests[0]['assets']
    expected = expected_assets(target_id, 'nightly')
    if len(assets) != len(expected):
        raise ValueError('carried asset count differs')
    for asset, (role, name, offset) in zip(assets, expected):
        if (
            not isinstance(asset, dict)
            or asset.get('role') != role
            or asset.get('name') != name
            or asset.get('offset') != offset
            or type(asset.get('size')) is not int
            or asset['size'] <= 0
            or not isinstance(asset.get('sha256'), str)
            or len(asset['sha256']) != 64
        ):
            raise ValueError('invalid carried asset metadata')
        url = urljoin(source_prefix, name)
        validate_url(url, index_url, 'nightly')
        data = fetch(url)
        if len(data) != asset['size'] or hashlib.sha256(data).hexdigest() != asset['sha256']:
            raise ValueError('carried asset failed size or SHA-256 verification')
        files[name] = data
    output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (output / name).write_bytes(data)
    return len(expected)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--previous-index', type=Path, required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--target', choices=TARGETS, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    previous = json.loads(args.previous_index.read_text())
    count = carry_target(previous, args.repository, args.target, args.output)
    print(f'Carried {args.target}: {count} unchanged assets verified')


if __name__ == '__main__':
    main()
