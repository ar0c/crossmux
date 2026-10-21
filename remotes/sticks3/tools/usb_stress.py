"""Bounded, read-only dual-USB regression for complete remote responses."""
import argparse
import json
from pathlib import Path
import sys
import time
from remote_debug import Remote

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mac', required=True)
parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--rounds', type=int, default=20)
parser.add_argument('--idle-ms', type=int, default=1000, help='Idle before each round, 0..1500 ms')
args = parser.parse_args()
if not 1 <= args.rounds <= 50: raise SystemExit('Rounds must be 1..50')
if not 0 <= args.idle_ms <= 1500: raise SystemExit('Idle must be 0..1500 ms')
if args.output.exists(): raise SystemExit('Evidence directory must be new')
sys.path.insert(0, str(args.repo/'scripts'))
from device_debug import Client, devices, device_id
ports = [p for p in devices() if device_id(p) == 'ff0b7263d6e8b324']
if len(ports) != 1: raise SystemExit('Exact Waveshare USB identity required')
reader = Client(ports[0].device, args.output)
remote = None
report = dict(success=False, rounds_completed=0, status_responses=0, screen_frames=0)
started = time.monotonic()
try:
    report['reader_identity'] = reader.hello()
    remote = Remote(args.mac)
    report['remote_identity'] = remote.hello()
    before = remote.command('STATUS')
    if not before['ready'] or before['busy']: raise RuntimeError('Remote not ready')
    for index in range(args.rounds):
        if time.monotonic()-started > 120: raise RuntimeError('Total deadline exceeded')
        time.sleep(args.idle_ms/1000)
        for _ in range(3):
            state = remote.command('STATUS')
            report['status_responses'] += 1
            if not state['ready'] or state['busy']: raise RuntimeError('Remote state changed')
        reader.command('SCREEN', screenshot=args.output/f'frame-{index:02d}.png')
        report['screen_frames'] += 1
        for _ in range(2):
            remote.command('STATUS')
            report['status_responses'] += 1
        report['rounds_completed'] += 1
    after = remote.command('STATUS')
    for key in ('issued','completed','dropped','reports_submitted','notify_errors'):
        if after[key] != before[key]: raise RuntimeError(f'Unexpected counter change: {key}')
    report.update(success=True, final_remote=after, final_reader=reader.command('STATUS'))
    reader.write_report(True)
except Exception as exc:
    report['error'] = str(exc)
    reader.write_report(False, str(exc), reader.evidence())
finally:
    report['elapsed_seconds'] = round(time.monotonic()-started, 3)
    if remote:
        report['remote_events'] = remote.events
        report['remote_partial_response'] = remote.partial.decode('utf-8', errors='replace')
        remote.close()
    reader.close()
    (args.output/'usb-stress.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k not in ('remote_events','remote_identity','reader_identity','final_remote','final_reader')},ensure_ascii=False))
raise SystemExit(0 if report['success'] else 1)
