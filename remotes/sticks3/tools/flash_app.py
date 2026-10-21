"""Flash only this verified StickS3's active application, preserving its rescue/data.

Requires a full backup that has already passed an independent verify-flash.
Never erases the chip, writes boot/partition/OTA metadata, or touches eFuses.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
from serial.tools import list_ports

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--firmware', type=Path, required=True)
parser.add_argument('--backup', type=Path, required=True)
parser.add_argument('--backup-sha256', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--previous-firmware', type=Path, help='For a verified upgrade, verify this prior app instead of the original whole-chip state')
args = parser.parse_args()
expected = '70:04:1d:db:aa:18'
ports = [p for p in list_ports.comports() if p.vid == 0x303a and p.pid == 0x1001 and
         (p.serial_number or '').lower() == expected]
if len(ports) != 1:
    raise SystemExit('Exact StickS3 USB identity required')
port = ports[0].device
backup = args.backup.read_bytes()
firmware = args.firmware.read_bytes()
if len(backup) != 0x800000 or hashlib.sha256(backup).hexdigest() != args.backup_sha256:
    raise SystemExit('Invalid full backup')
if not 0 < len(firmware) <= 0x3a0000 or firmware[0] != 0xe9:
    raise SystemExit('Invalid application image or size')
if args.output.exists():
    raise SystemExit('Preserve evidence: output must be new')
args.output.mkdir(parents=True)


def run(name, *arguments):
    with (args.output / f'{name}.log').open('w', encoding='utf-8') as log:
        result = subprocess.run([sys.executable,'-X','utf8','-m','esptool','--chip','esp32s3',
                                 '--port',port,'--baud','115200',*map(str,arguments)],
                                stdout=log,stderr=subprocess.STDOUT,timeout=180,
                                env={**os.environ,'PYTHONUTF8':'1','PYTHONIOENCODING':'utf-8'})
    if result.returncode:
        raise SystemExit(f'{name} failed; no retry; inspect {args.output}')
    return (args.output / f'{name}.log').read_text(encoding='utf-8')


identity = run('identity','flash-id').lower()
if expected not in identity or 'esp32-s3-pico-1' not in identity or 'detected flash size: 8mb' not in identity:
    raise SystemExit('Unexpected chip, MAC or Flash')
metadata = args.output / 'metadata-private.bin'
run('metadata','read-flash','--no-progress','0x8000','0x8000',metadata)
current = metadata.read_bytes()
if current[:0x1000] != backup[0x8000:0x9000] or current[0x6000:0x8000] != backup[0xe000:0x10000]:
    raise SystemExit('Partition table or OTA metadata changed since backup')
rows = [row for row in struct.iter_unpack('<HBBII16sI',backup[0x8000:0x8c00]) if row[0] == 0x50aa]
apps = [row for row in rows if row[1] == 0 and 0x10 <= row[2] <= 0x1f]
if len(apps) != 1 or apps[0][3:5] != (0xd0000,0x3a0000):
    raise SystemExit('Unexpected OTA layout')
sequence, = struct.unpack_from('<I', backup, 0xe000)
state, = struct.unpack_from('<I', backup, 0xe018)
if sequence != 1 or state != 2 or backup[0xf000:0xf020] != b'\xff'*32:
    raise SystemExit('Active OTA slot is not the verified valid reader slot')
boot = args.output / 'preserved-boot-table-private.bin'
rescue = args.output / 'preserved-ota-rescue-private.bin'
suffix = args.output / 'preserved-suffix-private.bin'
boot.write_bytes(backup[:0x9000])
rescue.write_bytes(backup[0xe000:0xd0000])
suffix.write_bytes(backup[0x470000:])
preserved = ('0x0',boot,'0xe000',rescue,'0x470000',suffix)
if args.previous_firmware:
    run('previous-app-verify','verify-flash','0xd0000',args.previous_firmware)
    run('before-preserved-verify','verify-flash',*preserved)
else:
    run('backup-verify','verify-flash','0x0',args.backup)
run('image-info','image-info',args.firmware)
print('Identity, backup and active reader partition verified; writing app only at 0xd0000',flush=True)
run('write','write-flash','--flash-mode','keep','--flash-freq','keep','--flash-size','keep',
    '0xd0000',args.firmware)
run('app-verify','verify-flash','0xd0000',args.firmware)
# NVS is deliberately writable: the radio updates calibration on first boot,
# and pairing subsequently writes bonds. It is backed up, never flashed here.
run('preserved-verify','verify-flash',*preserved)
(args.output / 'result.json').write_text(json.dumps({'success':True,'offset':'0xd0000',
    'bytes':len(firmware),'sha256':hashlib.sha256(firmware).hexdigest(),
    'app_verified':True,'boot_table_ota_rescue_spiffs_coredump_preserved':True,
    'nvs_write_by_flasher':False},indent=2))
print('Application, boot/table, OTA/rescue and SPIFFS/coredump verified; NVS remains mutable',flush=True)
