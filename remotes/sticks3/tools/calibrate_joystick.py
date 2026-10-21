"""Read-only observation/verification of requested physical directions; never writes calibration."""
import argparse
import json
from pathlib import Path
import time
from remote_debug import Remote
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--mac',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--directions',nargs='+',choices=('UP','DOWN','LEFT','RIGHT'),
               default=['UP','DOWN','LEFT','RIGHT'])
p.add_argument('--verify',action='store_true',help='Require each observed action to match the requested direction')
p.add_argument('--seconds',type=int,default=120,help='Human interaction window, 1..600 seconds')
a=p.parse_args()
if a.output.exists(): raise SystemExit('New evidence file required')
if not 1 <= a.seconds <= 600: raise SystemExit('Observation window must be 1..600 seconds')
if not 1 <= len(a.directions) <= 4 or len(set(a.directions)) != len(a.directions):
    raise SystemExit('1..4 distinct physical directions required')
r=Remote(a.mac)
report=dict(success=False,steps=[])
try:
    before=r.hello()
    deadline=time.monotonic()+a.seconds
    report['expected_directions']=a.directions
    print('Observe '+', '.join(a.directions)+', 2 seconds center between moves; do not click',flush=True)
    for desired in a.directions:
        while time.monotonic()<deadline:
            now=r.command('STATUS')
            if now['busy'] or now['completed']==before['completed']:
                time.sleep(.05); continue
            if now['completed']!=before['completed']+1 or now['joy_events']!=before['joy_events']+1 or now['last_action'] not in ('UP','DOWN','LEFT','RIGHT'):
                raise RuntimeError('Unexpected extra event; preserve capture and stop')
            report['steps'].append(dict(desired=desired,observed=now['last_action'],state=now))
            print(f"{desired}: observed {now['last_action']}",flush=True)
            if a.verify and now['last_action'] != desired:
                raise RuntimeError('Physical direction mismatch; preserve evidence and stop')
            before=now
            break
        else: raise RuntimeError('Calibration capture deadline')
    if len({x['observed'] for x in report['steps']})!=len(a.directions):
        raise RuntimeError('Distinct observed directions required')
    report['success']=True
except Exception as exc:
    report['error']=str(exc)
finally:
    report['requests']=r.requests
    report['events']=r.events
    a.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    r.close()
print(json.dumps(dict(success=report['success'],error=report.get('error'))))
raise SystemExit(0 if report['success'] else 1)
