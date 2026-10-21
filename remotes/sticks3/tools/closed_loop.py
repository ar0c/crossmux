"""Verify USB gesture -> real BLE -> CrossMux framebuffer, including reconnect.

Start on an interior page of the task-owned BLE-Loop-Test book, with mappings
already learned. Keeps the original page and USB keep-awake setting enabled.
Screenshots prove framebuffer state, not the optical appearance of the panel.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
from remote_debug import Remote

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mac',required=True)
parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[3])
parser.add_argument('--output',type=Path,required=True)
args = parser.parse_args()
sys.path.insert(0,str(args.repo/'scripts'))
from device_debug import Client, devices, device_id
if args.output.exists(): raise SystemExit('Evidence directory must be new')
ports = [p for p in devices() if device_id(p) == 'ff0b7263d6e8b324']
if len(ports) != 1: raise SystemExit('Exact Waveshare USB identity required')
reader = Client(ports[0].device,args.output)
remote = None
steps = []
report = {'success':False,'optical_acceptance':False,'physical_finger_press_verified':False}


def body_hash(path):
    # Reader raw frame is 800x480, rotated to portrait 480x800 by device_debug.
    # Physical x=80..703 covers portrait y=80..703, excluding the live footer clock.
    raw = path.with_suffix('.raw').read_bytes()
    if len(raw) != 48000: raise RuntimeError('Unexpected framebuffer size')
    return hashlib.sha256(b''.join(raw[y*100+10:y*100+88] for y in range(480))).hexdigest()


def snapshot(name):
    status = reader.wait(activity='EpubReader',timeout=30)
    path = args.output/f'{name}.png'
    reader.command('SCREEN',screenshot=path)
    return status,body_hash(path)


def gesture(name,button,duration,expected_activity=None,expected_body=None,changed_from=None):
    before = reader.wait(timeout=30)
    sent = remote.button(button,duration)
    after = reader.wait(activity=expected_activity,minimum_frame=before['frame']+1,timeout=30)
    deadline = time.monotonic()+10
    for probe in range(20):
        path = args.output/f'{name}-probe-{probe:02d}.png'
        reader.command('SCREEN',screenshot=path)
        content = body_hash(path)
        if ((expected_body is None or content == expected_body) and
                (changed_from is None or content != changed_from)):
            final = args.output/f'{name}.png'
            final.write_bytes(path.read_bytes())
            final.with_suffix('.raw').write_bytes(path.with_suffix('.raw').read_bytes())
            break
        if time.monotonic() >= deadline: raise RuntimeError(f'{name}: receiver content condition timeout; no replay')
        time.sleep(.2)
        after = reader.wait(activity=expected_activity,timeout=10)
    else: raise RuntimeError('Receiver content probe limit')
    result = dict(name=name,remote=sent,receiver_before=before,receiver_after=after,body_sha256=body_hash(path))
    steps.append(result)
    return result['body_sha256']


try:
    report['reader_identity'] = reader.hello()
    reader.command('KEEP',1)
    remote = Remote(args.mac)
    report['remote_identity'] = remote.hello()
    if not report['remote_identity']['ready']: raise RuntimeError('Remote not ready; finish book preparation first')
    initial,baseline = snapshot('00-baseline')
    if initial.get('sync_available') or initial.get('sync_active'):
        raise RuntimeError('Use the local fixture, not a WeRead book')
    report['baseline_body_sha256'] = baseline
    gesture('01-next','A',80,'EpubReader',changed_from=baseline)
    gesture('02-previous','B',80,'EpubReader',expected_body=baseline)
    gesture('03-confirm','B',800,changed_from=baseline)
    gesture('04-back','A',800,'EpubReader',expected_body=baseline)
    before = remote.command('STATUS')
    reader.logs.clear() # Host-side log window only; no device state is cleared.
    remote.command('DISCONNECT')
    deadline = time.monotonic()+60
    while time.monotonic() < deadline:
        after = remote.command('STATUS')
        if after['ready'] and after['connections'] > before['connections']: break
        time.sleep(.25)
    else: raise RuntimeError('Automatic reconnect timeout')
    report['reconnect'] = dict(before=before,after=after)
    # Peripheral subscription is earlier than the central's completed HID setup.
    # Wait for CrossMux's own connection transition, then its completed render.
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        state = reader.command('STATUS')
        if any('[BLE] connected:' in line for line in reader.logs) and state['ready'] and not state['pending']: break
        time.sleep(.2)
    else: raise RuntimeError('Receiver did not confirm reconnect readiness')
    report['reconnect']['receiver_ready'] = state
    gesture('05-reconnected-next','A',80,'EpubReader',changed_from=baseline)
    gesture('06-reconnected-previous','B',80,'EpubReader',expected_body=baseline)
    report['final_reader'] = reader.command('STATUS')
    report['final_remote'] = remote.command('STATUS')
    if not report['final_remote']['ready'] or report['final_remote']['busy']:
        raise RuntimeError('Remote not ready after verification')
    if report['final_remote']['notify_errors'] or report['final_remote']['dropped']:
        raise RuntimeError('HID error/drop counters are nonzero')
    report.update(success=True,receiver_verified=True,original_test_page_restored=True)
    reader.write_report(True)
except Exception as exc:
    report['error'] = str(exc)
    reader.write_report(False,str(exc),reader.evidence())
finally:
    report['steps'] = steps
    if remote:
        report['remote_events'] = remote.events
        report['remote_requests'] = remote.requests
        report['remote_partial_response'] = remote.partial.decode('utf-8',errors='replace')
        remote.close()
    reader.close()
    (args.output/'acceptance.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:report.get(k) for k in ['success','receiver_verified','original_test_page_restored','error']},ensure_ascii=False))
raise SystemExit(0 if report['success'] else 1)
