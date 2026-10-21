"""Back up and update only the active app of the exact Waveshare test reader.

No bootloader, partition, OTA selection, NVS or security writes. Never retries
an uncertain write. Private backup/logs stay in a new local evidence directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib
from serial.tools import list_ports

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--firmware', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--backup-from', type=Path, help='Reuse a fresh read-only full backup; live verify remains mandatory')
p.add_argument('--backup-sha256')
p.add_argument('--esptool-script', type=Path, required=True, help='Verified PlatformIO esptool 5.1.2 entry point for this reader')
a = p.parse_args()
expected = '44:b1:76:b7:ea:bc'
marker = b'CROSSPOINT-BOARD-V1:waveshare_epaper_397;'
image = a.firmware.read_bytes()
if image[0] != 0xe9 or struct.unpack_from('<H', image, 12)[0] != 9 or marker not in image:
    raise SystemExit('Waveshare ESP32-S3 image required')
if b'CROSSMUX-USB-DEBUG-V1;' not in image:
    raise SystemExit('Local closed-loop testing requires the USB debug image')
if a.output.exists(): raise SystemExit('Preserve evidence: output must be new')
a.output.mkdir(parents=True)
frozen_image = a.output / 'reader-app.bin'
frozen_image.write_bytes(image)


def run(name, *arguments):
    ports = [q for q in list_ports.comports() if q.vid == 0x303a and q.pid == 0x1001 and
             (q.serial_number or '').lower() == expected]
    if len(ports) != 1: raise SystemExit('Exact Waveshare USB identity required')
    print(name, flush=True)
    with (a.output / f'{name}.log').open('w', encoding='utf-8') as log:
        r = subprocess.run([sys.executable, '-X', 'utf8', str(a.esptool_script), '--chip', 'esp32s3',
                            '--port', ports[0].device, '--baud', '921600', *map(str, arguments)],
                           stdout=log, stderr=subprocess.STDOUT, timeout=600,
                           env={**os.environ, 'PYTHONUTF8':'1', 'PYTHONIOENCODING':'utf-8'})
    if r.returncode: raise SystemExit(f'{name} failed; inspect evidence, no automatic retry')
    return (a.output / f'{name}.log').read_text(encoding='utf-8')


identity = run('identity', 'flash-id').lower()
if expected not in identity or 'esp32-s3' not in identity or 'detected flash size: 16mb' not in identity:
    raise SystemExit('Unexpected chip/MAC/Flash')
security = run('security', 'get-security-info')
if 'Secure Boot: Disabled' not in security or 'Flash Encryption: Disabled' not in security:
    raise SystemExit('Security configuration prevents this ordinary reversible app update')
backup_path = a.output / 'reader-before-private.bin'
if a.backup_from:
    candidate = a.backup_from.read_bytes()
    if not a.backup_sha256 or hashlib.sha256(candidate).hexdigest() != a.backup_sha256:
        raise SystemExit('Backup digest mismatch')
    backup_path.write_bytes(candidate)
else:
    run('backup', 'read-flash', '--no-progress', '0', '0x1000000', backup_path)
raw = backup_path.read_bytes()
if len(raw) != 0x1000000: raise SystemExit('Incomplete backup')
run('backup-verify', 'verify-flash', '0', backup_path)
parts = {}
for off in range(0x8000, 0x9000, 32):
    magic, typ, sub, address, size, label, flags = struct.unpack_from('<HBBII16sI', raw, off)
    if magic != 0x50aa: break
    parts[label.rstrip(b'\0').decode('ascii')] = (typ, sub, address, size)
for label, expected_part in {'nvs':(1,2,0x9000,0x5000), 'otadata':(1,0,0xe000,0x2000),
                            'app0':(0,0x10,0x10000,0x640000), 'app1':(0,0x11,0x650000,0x640000),
                            'spiffs':(1,0x82,0xc90000,0x360000), 'coredump':(1,3,0xff0000,0x10000)}.items():
    if parts.get(label) != expected_part: raise SystemExit(f'Unexpected partition {label}')
sequences = []
for off in (0xe000, 0xf000):
    seq, _, state, crc = struct.unpack_from('<I20sII', raw, off)
    if seq != 0xffffffff and state == 2 and crc == zlib.crc32(struct.pack('<I',seq),0xffffffff):
        sequences.append(seq)
if not sequences: raise SystemExit('No validated active OTA selection')
slot = (max(sequences)-1) % 2
_, _, address, size = parts[f'app{slot}']
old = raw[address:address+size]
if old[0] != 0xe9 or marker not in old or len(image) > size:
    raise SystemExit('Current application identity or new app size mismatch')
plan = dict(active_slot=slot, address=address, image_bytes=len(image),
            image_sha256=hashlib.sha256(image).hexdigest(), backup_sha256=hashlib.sha256(raw).hexdigest())
(a.output/'plan.json').write_text(json.dumps(plan,indent=2))
preserved = []
for label, start, end in (('boot-table',0,0x9000), ('ota',0xe000,0x10000),
                          ('before-active',0x10000,address), ('after-active',address+size,len(raw))):
    if end == start: continue
    path = a.output/f'{label}-private.bin'
    path.write_bytes(raw[start:end])
    preserved += [hex(start), path]
run('write-app', 'write-flash', '--flash-mode','keep','--flash-freq','keep','--flash-size','keep',hex(address),frozen_image)
run('verify-app', 'verify-flash', hex(address), frozen_image)
run('verify-preserved', 'verify-flash', *preserved)
(a.output/'result.json').write_text(json.dumps(dict(success=True, **plan,
    app_verified=True, boot_table_ota_inactive_app_data_preserved=True, nvs_written_by_flasher=False),indent=2))
print('Application and all preserved regions verified', flush=True)
