"""Flash the retained, previously verified application without rebuilding.

--check validates local files only. Actual flashing delegates to flash_app.py;
no erase, retry, partition write or whole-chip restore is added here.
"""
import argparse
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import subprocess
import sys
from datetime import datetime


def verified_images(folder):
    folder = folder.resolve(strict=True)
    manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8-sig'))
    if manifest.get('format') != 1:
        raise ValueError('Unsupported saved firmware manifest')
    images = {}
    for key in ('application', 'original_flash'):
        record = manifest[key]
        path = (folder / record['file']).resolve(strict=True)
        if path.parent != folder or not path.is_file():
            raise ValueError('Firmware must be a file inside the saved firmware folder')
        payload = path.read_bytes()
        if len(payload) != record['bytes'] or hashlib.sha256(payload).hexdigest() != record['sha256']:
            raise ValueError('Saved firmware size or SHA-256 mismatch: ' + key)
        if key == 'original_flash' and len(payload) != 0x800000:
            raise ValueError('Expected the verified 8 MiB original Flash image')
        if key == 'application' and (not payload or payload[0] != 0xe9 or len(payload) > 0x3a0000):
            raise ValueError('Invalid application image')
        images[key] = path
    return manifest, images


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Validate files and Python tools; no device access')
    args = parser.parse_args(argv)
    project = Path(__file__).resolve().parents[1]
    manifest, images = verified_images(project / 'firmware')
    if version('esptool') != '5.3.0' or version('pyserial') != '3.5':
        raise ValueError('Use Python with esptool==5.3.0 and pyserial==3.5; see requirements.txt')
    if args.check:
        print('Saved firmware, original Flash and Python tools verified; no device access')
        return 0
    output = project / 'artifacts' / ('flash-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    # The retained application is also the known installed baseline. A different
    # current application must fail the existing pre-write comparison.
    command = [sys.executable, str(project / 'tools/flash_app.py'),
               '--firmware', str(images['application']),
               '--previous-firmware', str(images['application']),
               '--backup', str(images['original_flash']),
               '--backup-sha256', manifest['original_flash']['sha256'],
               '--output', str(output)]
    return subprocess.run(command, check=False).returncode


if __name__ == '__main__':
    sys.exit(main())
