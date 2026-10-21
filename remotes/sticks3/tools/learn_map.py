"""Learn four keys through real BLE, starting on CrossMux's first mapping row.

Requires the dedicated Waveshare USB debug firmware and an already paired remote.
This updates the four remote mappings in the reader's persistent settings.
"""
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
args = parser.parse_args()
sys.path.insert(0,str(args.repo/'scripts'))
from device_debug import Client, devices, device_id
if args.output.exists(): raise SystemExit('Preserve evidence: output must be new')
ports = [p for p in devices() if device_id(p) == 'ff0b7263d6e8b324']
if len(ports) != 1: raise SystemExit('Exact Waveshare USB identity required')
reader = Client(ports[0].device,args.output)
remote = None
try:
    reader.hello()
    initial = reader.wait(activity='BleButtonMap')
    remote = Remote(args.mac)
    if not remote.hello()['ready']: raise RuntimeError('Remote not ready')
    steps = []
    for index,(action,button,duration) in enumerate((('NEXT','A',80),('PREV','B',80),('CONFIRM','B',800),('BACK','A',800))):
        if index:
            reader.command('PULSE','DOWN',60)
            reader.wait(activity='BleButtonMap')
        reader.command('PULSE','PRESS',60)
        before = reader.wait(activity='BleButtonMap')
        result = remote.button(button,duration)
        after = reader.wait(activity='BleButtonMap',minimum_frame=before['frame']+1)
        reader.command('SCREEN',screenshot=args.output/f'mapped-{index}-{action}.png')
        if result['last_action'] != action: raise RuntimeError('Unexpected action')
        steps.append(dict(action=action,remote=result,receiver_before=before,receiver_after=after))
    reader.write_report(True)
    (args.output/'learning.json').write_text(json.dumps({'success':True,'steps':steps,'remote_events':remote.events},indent=2))
    print(json.dumps({'success':True,'learned':[step['action'] for step in steps]}))
except Exception as exc:
    reader.write_report(False,str(exc),reader.evidence())
    print(json.dumps({'success':False,'error':str(exc)}))
    raise SystemExit(1)
finally:
    if remote:
        (args.output/'remote-events.json').write_text(json.dumps(remote.events,indent=2))
    reader.close()
    if remote: remote.close()
