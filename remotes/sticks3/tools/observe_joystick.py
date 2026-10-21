"""Read-only capture of an explicit, balanced physical horizontal gesture sequence."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
from remote_debug import Remote

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--mac',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--seconds',type=int,default=90)
p.add_argument('--directions',nargs='+',choices=('LEFT','RIGHT'),default=['RIGHT','LEFT'],
               help='2..8 expected physical gestures, balanced to restore the initial page')
p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[3])
a=p.parse_args()
if not 1 <= a.seconds <= 120 or a.output.exists(): raise SystemExit('New evidence folder and 1..120 seconds required')
if not 2 <= len(a.directions) <= 8 or a.directions.count('LEFT') != a.directions.count('RIGHT'):
    raise SystemExit('2..8 balanced LEFT/RIGHT gestures required')
sys.path.insert(0,str(a.repo/'scripts'))
from device_debug import Client,devices,device_id
ports=[q for q in devices() if device_id(q)=='ff0b7263d6e8b324']
if len(ports)!=1: raise SystemExit('Exact reader required')
reader=Client(ports[0].device,a.output)
remote=None
report=dict(success=False,origin='physical joystick; host sends HELLO/STATUS/SCREEN only',
            expected_directions=a.directions,steps=[])
def body(path):
    raw=path.with_suffix('.raw').read_bytes()
    return hashlib.sha256(b''.join(raw[y*100+10:y*100+88] for y in range(480))).hexdigest()
try:
    reader.hello()
    state=reader.wait(activity='EpubReader')
    if state['sync_available'] or state['sync_active']: raise RuntimeError('Local fixture required')
    reader.command('SCREEN',screenshot=a.output/'00-before.png')
    baseline=body(a.output/'00-before.png')
    remote=Remote(a.mac)
    before=remote.hello()
    if not before['ready'] or not before['joy_online'] or before['busy']: raise RuntimeError('Joystick not ready')
    report['initial_remote']=before
    print('Observing physical '+', '.join(a.directions)+'; no injected controls',flush=True)
    deadline=time.monotonic()+a.seconds
    displacement=0
    for index,expected in enumerate(a.directions,1):
        while time.monotonic()<deadline:
            current=remote.command('STATUS')
            if current['busy'] or current['completed']==before['completed']:
                time.sleep(.15); continue
            if current['completed']!=before['completed']+1 or current['joy_events']!=before['joy_events']+1 or current['last_action']!=expected:
                raise RuntimeError('Unexpected physical event; inspect evidence, no replay')
            after=reader.wait(activity='EpubReader',minimum_frame=state['frame']+1)
            path=a.output/f'{index:02d}-{expected}.png'
            reader.command('SCREEN',screenshot=path)
            content=body(path)
            displacement += 1 if expected=='RIGHT' else -1
            if (displacement!=0 and content==baseline) or (displacement==0 and content!=baseline):
                raise RuntimeError('Physical action did not change/restore the receiver body')
            report['steps'].append(dict(expected=expected,remote=current,reader=after,body_sha256=content))
            print(f'Physical {expected} verified',flush=True)
            before=current; state=after
            break
        else: raise RuntimeError('No expected physical gesture before deadline')
    report['success']=True
    report['final_remote']=remote.command('STATUS')
    report['final_reader']=reader.command('STATUS')
except Exception as exc:
    report['error']=str(exc)
    report['evidence']=reader.evidence()
finally:
    reader.write_report(report['success'],report.get('error'))
    report['remote_requests']=remote.requests if remote else []
    report['remote_events']=remote.events if remote else []
    (a.output/'physical.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    reader.close()
    if remote: remote.close()
print(json.dumps(dict(success=report['success'],error=report.get('error'))))
raise SystemExit(0 if report['success'] else 1)
