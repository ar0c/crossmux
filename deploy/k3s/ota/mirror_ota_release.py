#!/usr/bin/env python3
"""Mirror verified public fork releases into a local, read-only OTA tree.

The rolling index is replaced only after every immutable manifest and binary
has been downloaded and verified. Existing build directories are never edited.
"""

import argparse
import hashlib
import json
import os
import re
import tempfile
import time
import urllib.request
from pathlib import Path
from urllib.parse import urlparse

SOURCE = 'https://github.com/ar0c/crossmux/releases/download/'
DESTINATION = 'https://ooo.ar0c.com/releases/download/'
TARGETS = {
    'nightly': {'xteink_x4_pro': ('x4pro', 'x4pro'),
                'waveshare_epaper_397': ('waveshare_epaper_397', 'waveshare-epaper-397')},
    'stable': {'xteink_x4_pro': ('x4pro', 'x4pro')},
}
BUILD_TAG = re.compile(r'^(nightly|stable)-build-[0-9a-f]{40}-[0-9]+-[0-9]+$')
FILENAME = re.compile(r'^[a-z0-9][a-z0-9._-]{0,127}$')
SHA256 = re.compile(r'^[0-9a-f]{64}$')
MAX_JSON = 65536
MAX_BINARY = 16 * 1024 * 1024


def fetch(url, limit):
    request = urllib.request.Request(url, headers={'User-Agent': 'crossmux-ar0c-mirror/1'})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f'asset exceeds {limit} bytes: {url}')
    return data


def source_asset_url(url, channel):
    if not url.startswith(SOURCE):
        raise ValueError('manifest URL is outside fork releases')
    path = url[len(SOURCE):]
    parts = path.split('/')
    if len(parts) != 2 or not BUILD_TAG.fullmatch(parts[0]) or not parts[0].startswith(channel + '-'):
        raise ValueError('manifest URL is not in an immutable channel build')
    if not FILENAME.fullmatch(parts[1]) or urlparse(url).query or urlparse(url).fragment:
        raise ValueError('unsafe manifest URL')
    return parts


def mirror_channel(root, channel):
    index_url = SOURCE + channel + '/release-index.json'
    index = json.loads(fetch(index_url, MAX_JSON))
    if index.get('schemaVersion') != 1 or index.get('channel') != channel:
        raise ValueError('invalid rolling index')
    targets = index.get('targets')
    if not isinstance(targets, dict) or set(targets) != set(TARGETS[channel]):
        raise ValueError('unexpected target set')
    build_id = index.get('buildId')
    if not isinstance(build_id, str) or not BUILD_TAG.fullmatch(build_id) or not build_id.startswith(channel + '-'):
        raise ValueError('invalid build ID')

    root.mkdir(parents=True, exist_ok=True)
    current = root / channel / 'release-index.json'
    if current.is_file() and (root / build_id).is_dir():
        published = json.loads(current.read_bytes())
        for entry in published.get('targets', {}).values():
            for pointer in entry.get('variants', {}).values():
                url = pointer.get('manifestUrl', '')
                if url.startswith(DESTINATION):
                    pointer['manifestUrl'] = SOURCE + url[len(DESTINATION):]
        if published == index:
            return build_id, 0

    with tempfile.TemporaryDirectory(prefix='.staging-', dir=root) as temp:
        staging = Path(temp)
        mirrored = {}
        for target_id, entry in targets.items():
            board_tag, slug = TARGETS[channel][target_id]
            if (entry.get('targetId') != target_id or entry.get('boardTag') != board_tag or
                    not isinstance(entry.get('variants'), dict)):
                raise ValueError('invalid target entry')
            if set(entry['variants']) != {'global', 'zh-CN'}:
                raise ValueError('incomplete content variants')
            for flavor, pointer in entry['variants'].items():
                url = pointer.get('manifestUrl')
                tag, name = source_asset_url(url, channel)
                if tag != build_id:
                    raise ValueError('manifest points to another build')
                suffix = 'global' if flavor == 'global' else 'cn'
                if name != f'{slug}-{suffix}-manifest.json':
                    raise ValueError('unexpected manifest filename')
                manifest_bytes = fetch(url, MAX_JSON)
                manifest = json.loads(manifest_bytes)
                if any(manifest.get(key) != value for key, value in (
                    ('schemaVersion', 1), ('channel', channel), ('targetId', target_id),
                    ('flavor', flavor), ('boardTag', board_tag),
                    ('version', pointer.get('version')), ('crossmuxSha', pointer.get('crossmuxSha')),
                    ('sdkSha', pointer.get('sdkSha')),
                )):
                    raise ValueError('manifest does not match index')
                assets = manifest.get('assets')
                if not isinstance(assets, list) or not assets or not any(a.get('role') == 'firmware' for a in assets):
                    raise ValueError('manifest has no firmware')
                for asset in assets:
                    filename, size, digest = asset.get('name'), asset.get('size'), asset.get('sha256')
                    if (not isinstance(filename, str) or not FILENAME.fullmatch(filename) or
                            not filename.startswith(f'crossmux-ar0c-{slug}-') or type(size) is not int or
                            not 0 < size <= MAX_BINARY or not isinstance(digest, str) or
                            not SHA256.fullmatch(digest)):
                        raise ValueError('invalid firmware asset metadata')
                    if filename not in mirrored:
                        data = fetch(SOURCE + tag + '/' + filename, MAX_BINARY)
                        if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
                            raise ValueError(f'firmware asset failed SHA-256: {filename}')
                        (staging / filename).write_bytes(data)
                        mirrored[filename] = (size, digest)
                    elif mirrored[filename] != (size, digest):
                        raise ValueError('asset differs between manifests')
                (staging / name).write_bytes(manifest_bytes)
                pointer['manifestUrl'] = DESTINATION + tag + '/' + name

        build_dir = root / build_id
        if build_dir.exists():
            for file in staging.iterdir():
                if not (build_dir / file.name).is_file() or (build_dir / file.name).read_bytes() != file.read_bytes():
                    raise ValueError(f'immutable build differs: {file.name}')
        else:
            os.replace(staging, build_dir)

    channel_dir = root / channel
    channel_dir.mkdir(exist_ok=True)
    index_bytes = (json.dumps(index, ensure_ascii=False, indent=2) + '\n').encode()
    with tempfile.NamedTemporaryFile(prefix='.index-', dir=channel_dir, delete=False) as pending:
        try:
            pending.write(index_bytes)
            pending.flush()
            os.fsync(pending.fileno())
            pending_name = pending.name
        except BaseException:
            Path(pending.name).unlink(missing_ok=True)
            raise
    os.replace(pending_name, channel_dir / 'release-index.json')
    return build_id, len(mirrored)


def mirror_with_lock(root, channel, timeout=60):
    """Serialize scheduled and release-triggered updates of the same tree."""
    import fcntl  # Linux-only K3s workload; plain mirror_channel stays testable on Windows.
    root.mkdir(parents=True, exist_ok=True)
    with (root / '.mirror.lock').open('a+b') as lock:
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('another OTA mirror is still running')
                time.sleep(1)
        try:
            return mirror_channel(root, channel)
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--channel', choices=TARGETS, required=True)
    args = parser.parse_args()
    build_id, assets = mirror_with_lock(args.root, args.channel)
    if assets:
        print(f'{args.channel}: {build_id}, {assets} unique assets verified')
    else:
        print(f'{args.channel}: {build_id}, unchanged')


if __name__ == '__main__':
    main()
