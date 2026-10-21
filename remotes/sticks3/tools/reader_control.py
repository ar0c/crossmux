"""Run bounded CrossMux USB debug steps; preserves evidence and never retries writes."""
import argparse
import json
from pathlib import Path
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--steps', type=Path, required=True, help='JSON array of device_debug scenario steps')
args = parser.parse_args()
sys.path.insert(0, str(args.repo / 'scripts'))
from device_debug import Client, devices, device_id, run_scenario


def wait_ready(self, *, activity=None, timeout=90, minimum_frame=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = self.command('STATUS', timeout=min(30, max(.1, deadline-time.monotonic())))
        if (status['ready'] and not status['pending'] and
                (activity is None or status['activity'] == activity) and
                (minimum_frame is None or status['frame'] >= minimum_frame)):
            return status
        time.sleep(.3)
    raise RuntimeError('Receiver ready deadline exceeded; no replay')


Client.wait = wait_ready
if args.output.exists():
    raise SystemExit('Evidence directory exists; use a fresh name')
ports = [p for p in devices() if device_id(p) == 'ff0b7263d6e8b324']
if len(ports) != 1:
    raise SystemExit('Expected exactly one verified Waveshare USB identity')
client = Client(ports[0].device, args.output)
try:
    client.hello()
    result = run_scenario(client, {'steps': json.loads(args.steps.read_text(encoding='utf-8-sig'))}, 300)
    client.write_report(True)
    print(json.dumps({'success': True, 'result': result[-1]}, ensure_ascii=False))
except Exception as exc:
    client.write_report(False, str(exc), client.evidence())
    print(json.dumps({'success': False, 'error': str(exc)}, ensure_ascii=False))
    raise SystemExit(1)
finally:
    client.close()
