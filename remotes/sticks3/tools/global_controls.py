"""Run explicit BLE gestures and capture receiver evidence; never replay a mutation.

Steps JSON is an array of {control: UP/DOWN/LEFT/RIGHT/CLICK/A/B,
duration_ms: 100, activity: optional expected name, local_book: optional bool}.
The tool verifies input submissions plus a completed receiver frame. Inspect
screenshots for the intended behavior; a changed frame alone is not acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
from remote_debug import Remote

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--mac', required=True)
p.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
p.add_argument('--steps', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
steps = json.loads(a.steps.read_text(encoding='utf-8-sig'))
if not isinstance(steps,list) or not 1 <= len(steps) <= 30: raise SystemExit('1..30 steps required')
for step in steps:
    if step.get('control') not in ('UP','DOWN','LEFT','RIGHT','CLICK','A','B'):
        raise SystemExit('Unsupported control')
    if type(step.get('duration_ms',100)) is not int or not 30 <= step.get('duration_ms',100) <= 2000:
        raise SystemExit('Duration must be 30..2000 ms')
if a.output.exists(): raise SystemExit('Evidence directory must be new')
sys.path.insert(0,str(a.repo/'scripts'))
from device_debug import Client, devices, device_id
ports = [q for q in devices() if device_id(q) == 'ff0b7263d6e8b324']
if len(ports) != 1: raise SystemExit('Exact Waveshare USB identity required')
reader = Client(ports[0].device, a.output)
remote = None
report = dict(success=False, steps=[])
try:
    report['reader_identity'] = reader.hello()
    reader.command('KEEP',1)
    remote = Remote(a.mac)
    report['remote_identity'] = remote.hello()
    if not report['remote_identity']['ready']: raise RuntimeError('Remote not ready')
    reader.wait(timeout=30)
    reader.command('SCREEN',screenshot=a.output/'00-before.png')
    for index,step in enumerate(steps,1):
        before = reader.wait(timeout=30)
        control = step['control']
        duration = step.get('duration_ms',100)
        sent = remote.button(control,duration) if control in ('A','B') else remote.joystick(control,duration)
        after = reader.wait(activity=step.get('activity'),minimum_frame=before['frame']+1,timeout=30)
        if step.get('local_book') and (after['sync_available'] or after['sync_active']):
            raise RuntimeError('Local book required; stop before further reading actions')
        path = a.output/f'{index:02d}-{control}.png'
        reader.command('SCREEN',screenshot=path)
        report['steps'].append(dict(control=control,duration_ms=duration,remote=sent,before=before,after=after,
                                  frame_sha256=hashlib.sha256(path.with_suffix('.raw').read_bytes()).hexdigest()))
        print(json.dumps(dict(step=index,control=control,activity=after['activity'],frame=after['frame'])),flush=True)
    report['final_reader'] = reader.command('STATUS')
    report['final_remote'] = remote.command('STATUS')
    report['success'] = True
except Exception as exc:
    report['error'] = str(exc)
    report['failure_evidence'] = reader.evidence()
finally:
    reader.write_report(report['success'],report.get('error'))
    report['remote_events'] = remote.events if remote else []
    (a.output/'controls.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    reader.close()
    if remote: remote.close()
print(json.dumps(dict(success=report['success'],error=report.get('error'))))
raise SystemExit(0 if report['success'] else 1)
