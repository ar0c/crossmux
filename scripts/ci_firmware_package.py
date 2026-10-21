"""Seal and independently verify a complete CI firmware package before promotion."""

import configparser
import json
import re
from pathlib import Path

from build_nightly_index import valid_manifest
from nightly_targets import FLAVOR_TOKENS, TARGETS, asset_name, environment_for, manifest_name, version_for
from package_nightly_target import (
    EXPECTED_PARTITIONS, FULL_INSTALL_SEGMENTS, OTA_SEGMENT, git_value, sha256,
    verify_firmware, verify_partition_csv,
)


TARGET = 'waveshare_epaper_397'
SHA40 = re.compile(r'[0-9a-f]{40}')


def provenance_name():
    return TARGETS[TARGET]['deviceSlug'] + '-ci-provenance.json'


def package_names():
    segments = FULL_INSTALL_SEGMENTS if TARGETS[TARGET]['fullInstall'] else OTA_SEGMENT
    return {asset_name(TARGET, name) for _role, name, _offset in segments} | {
        manifest_name(TARGET, flavor) for flavor in FLAVOR_TOKENS
    } | {TARGETS[TARGET]['deviceSlug'] + '-SHA256SUMS'}


def expected_identity(root):
    config = configparser.ConfigParser()
    config.read(root / 'platformio.ini', encoding='utf-8')
    revision = git_value(root, 'rev-parse', 'HEAD')
    sdk = git_value(root, 'rev-parse', 'HEAD:freeink-sdk')
    if not SHA40.fullmatch(revision) or not SHA40.fullmatch(sdk):
        raise ValueError('invalid checkout or SDK revision')
    return revision, sdk, version_for(config['crosspoint']['version'], TARGET, 'nightly', 'global', revision)


def verify_package(root, package):
    revision, sdk, version = expected_identity(root)
    verify_partition_csv(root)
    names = package_names()
    actual = {path.name for path in package.iterdir()}
    if actual - {provenance_name()} != names or any(not path.is_file() or path.is_symlink() for path in package.iterdir()):
        raise ValueError('CI package has missing, extra or non-regular files')
    manifests = []
    segments = FULL_INSTALL_SEGMENTS if TARGETS[TARGET]['fullInstall'] else OTA_SEGMENT
    for flavor in FLAVOR_TOKENS:
        manifest = json.loads((package / manifest_name(TARGET, flavor)).read_text(encoding='utf-8'))
        if not valid_manifest(manifest, TARGET, flavor, 'nightly'):
            raise ValueError('invalid compatibility manifest')
        for key, value in {'crossmuxSha': revision, 'sdkSha': sdk, 'version': version,
                           'environment': environment_for(TARGET, 'nightly', flavor)}.items():
            if manifest.get(key) != value:
                raise ValueError('CI package identity mismatch: ' + key)
        if manifest.get('sourceKind') is not None:
            raise ValueError('local diagnostic packages cannot be promoted')
        if len(manifest['assets']) != len(segments):
            raise ValueError('incomplete CI asset set')
        for asset, (role, source, offset) in zip(manifest['assets'], segments):
            path = package / asset_name(TARGET, source)
            if (asset.get('name') != path.name or asset.get('role') != role or asset.get('offset') != offset
                    or asset.get('size') != path.stat().st_size or asset.get('sha256') != sha256(path)):
                raise ValueError('CI asset identity, size or SHA-256 mismatch')
        manifests.append({key: value for key, value in manifest.items() if key != 'flavor'})
    if manifests[0] != manifests[1]:
        raise ValueError('compatibility manifests differ beyond flavor')
    firmware = package / asset_name(TARGET, 'firmware.bin')
    if firmware.stat().st_size > EXPECTED_PARTITIONS['app0'][1]:
        raise ValueError('CI firmware exceeds OTA slot')
    verify_firmware(firmware, TARGETS[TARGET]['chipId'], TARGETS[TARGET]['boardTag'])
    if version.encode('ascii') + b'\0' not in firmware.read_bytes():
        raise ValueError('CI firmware does not embed the expected version')
    checksum = TARGETS[TARGET]['deviceSlug'] + '-SHA256SUMS'
    expected_sums = {name: sha256(package / name) for name in names - {checksum}}
    rows = (package / checksum).read_text(encoding='ascii').splitlines()
    if sorted(rows) != sorted(f'{digest}  {name}' for name, digest in expected_sums.items()):
        raise ValueError('CI checksum file is incomplete or inconsistent')
    return revision, sdk, version


def context(repository, ref, run_id, attempt):
    if not isinstance(repository, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('invalid repository')
    if (not isinstance(ref, str) or not re.fullmatch(r'refs/(?:heads/[^\r\n]+|tags/[^\r\n]+|pull/[1-9][0-9]*/(?:merge|head))', ref)):
        raise ValueError('invalid workflow ref')
    if type(run_id) is not int or run_id <= 0 or type(attempt) is not int or attempt <= 0:
        raise ValueError('invalid CI run identity')
    return {'repository': repository, 'ref': ref, 'runId': run_id, 'runAttempt': attempt}


def seal(root, package, repository, ref, run_id, attempt):
    revision, sdk, version = verify_package(root, package)
    record = dict(schemaVersion=1, **context(repository, ref, run_id, attempt),
                  crossmuxSha=revision, sdkSha=sdk, version=version, targetId=TARGET,
                  environment=environment_for(TARGET, 'nightly', 'global'))
    record['files'] = {name: {'size': (package / name).stat().st_size, 'sha256': sha256(package / name)}
                       for name in sorted(package_names())}
    path = package / provenance_name()
    if path.exists():
        raise ValueError('CI package is already sealed')
    path.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')


def verify_seal(root, package, repository, ref, run_id, attempt):
    revision, sdk, version = verify_package(root, package)
    record = json.loads((package / provenance_name()).read_text(encoding='utf-8'))
    expected = dict(schemaVersion=1, **context(repository, ref, run_id, attempt),
                    crossmuxSha=revision, sdkSha=sdk, version=version, targetId=TARGET,
                    environment=environment_for(TARGET, 'nightly', 'global'))
    expected['files'] = {name: {'size': (package / name).stat().st_size, 'sha256': sha256(package / name)}
                         for name in sorted(package_names())}
    if record != expected:
        raise ValueError('CI provenance does not match the selected run or package')
    return record
