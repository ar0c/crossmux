"""Run bounded desktop UI smoke tests with isolated SD fixtures and screenshots.

Build simulator and simulator_waveshare_ui first. The latter verifies Waveshare
UI geometry only; its underlying pinned HAL uses the non-touch 800x480 profile.
No WeRead credentials, real upload, firmware installation or deployment.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path, required=True, help='new Windows-accessible evidence directory')
    parser.add_argument('--profiles', default='simulator,simulator_waveshare_ui')
    args = parser.parse_args()
    root, out = args.root.resolve(), args.out.resolve()
    if os.name != 'nt' and not re.match(r'^/mnt/[a-z]/', str(out)):
        parser.error('WSL screenshots must be saved on the Windows filesystem')
    profiles = args.profiles.split(',')
    if any(p not in ('simulator','simulator_waveshare_ui') for p in profiles):
        parser.error('unsupported acceptance profile')
    out.mkdir(parents=True, exist_ok=False)
    results = []
    report = {'status':'running','results':results,'real_uploads':0,'real_logins':0,
              'boundary':'desktop UI/lifecycle only; no hardware timing, TLS/SD/PSRAM or cloud-credit proof'}
    try:
        for profile in profiles:
            binary = root / '.pio/build' / profile / 'program'
            if not binary.is_file():
                raise RuntimeError(f'build {profile} first')
            folder = out / profile
            sd = folder / 'fixture-sd'
            (sd / '.crosspoint').mkdir(parents=True)
            (sd / '.crosspoint/settings.json').write_text(json.dumps({
                'language':'ZH_CN','onboardingVersion':1,'uiTheme':5,'shortPwrBtn':0,
                'bluetoothEnabled':0}),encoding='utf8')
            shutil.copy2(binary,folder/'program-linux')
            env = os.environ.copy()
            env.update({
                'CROSSPOINT_SIM_SD':str(sd),
                'CROSSPOINT_SIM_INPUT_SCRIPT':'2500:RIGHT:80;2800:RIGHT:80;3100:RIGHT:80;5000:SLEEP;6500:POWER:600',
                'CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE':'3000:QUIT',
                'CROSSPOINT_SIM_SCREENSHOTS':f'2000:{folder}/home.bmp;4000:{folder}/settings.bmp',
                'CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE':f'2200:{folder}/wake.bmp',
                'CROSSPOINT_SIM_HTTP_PORT':'0',
            })
            started = time.monotonic()
            with (folder/'stdout.log').open('w') as stdout, (folder/'stderr.log').open('w') as stderr:
                result = subprocess.run([str(binary)],cwd=root,env=env,stdout=stdout,stderr=stderr,timeout=25)
            if result.returncode:
                raise RuntimeError(f'{profile} exited {result.returncode}; inspect its logs')
            log=(folder/'stdout.log').read_text(errors='replace')+(folder/'stderr.log').read_text(errors='replace')
            if 'Entering deep sleep' not in log:
                raise RuntimeError(f'{profile} never entered the requested sleep state')
            screenshots = []
            for name in ('home','settings','wake'):
                file = folder/f'{name}.bmp'
                data = file.read_bytes()
                if data[:2]!=b'BM' or len(data)<54:
                    raise RuntimeError(f'invalid screenshot {file}')
                width,height = struct.unpack_from('<ii',data,18)
                if width != 480 or abs(height) != 800:
                    raise RuntimeError(f'wrong portrait dimensions: {width}x{height}')
                offset=struct.unpack_from('<I',data,10)[0]
                pixels=data[offset:]
                if len(set(pixels))<2:
                    raise RuntimeError(f'blank screenshot: {file}')
                screenshots.append({'scene':name,'path':str(file),'sha256':hashlib.sha256(data).hexdigest(),'width':width,'height':abs(height)})
            if screenshots[0]['sha256']==screenshots[1]['sha256']:
                raise RuntimeError(f'{profile} did not visibly navigate to settings')
            results.append({'profile':profile,'status':'passed','elapsed_ms':round((time.monotonic()-started)*1000),
                            'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                            'scope':'Waveshare button/UI geometry; non-touch host HAL alias' if profile.endswith('_ui') else 'X4 Pro native profile',
                            'screenshots':screenshots})
        report['status']='passed'
    except Exception as error:
        report['status']='failed'
        report['error']=str(error)
        raise
    finally:
        (out/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
    print(f'PASS {len(results)} simulator profiles; report={out / "result.json"}')


if __name__=='__main__':
    main()
