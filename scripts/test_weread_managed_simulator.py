"""Accept the production managed-reading pipeline with a synthetic loopback backend.

Build simulator_managed_acceptance and run weread-sync cmd/local-acceptance first.
The separate host transport cannot compile into either hardware image.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from urllib.parse import urlparse
import zipfile
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    meta = json.loads(args.fixture.read_text())
    target = urlparse(meta['url'])
    if target.scheme != 'http' or target.hostname != '127.0.0.1' or meta['real_uploads'] or meta['real_logins']:
        parser.error('synthetic loopback fixture required')
    if os.name != 'nt' and not str(args.out.resolve()).startswith('/mnt/'):
        parser.error('evidence must be stored on the Windows filesystem')
    args.out.mkdir(parents=True, exist_ok=False)
    sd = args.out / 'device-sd'
    (sd / 'WeReadSync').mkdir(parents=True)
    (sd / 'WeReadSync/service.conf').write_text('123\n'+meta['device_id']+'\n'+meta['device_token']+'\n')
    binary = args.root / '.pio/build/simulator_managed_acceptance/program'
    env = os.environ.copy()
    env.update(CROSSPOINT_SIM_SD=str(sd), CROSSPOINT_MANAGED_FIXTURE=meta['url'], CROSSPOINT_SIM_HTTP_PORT='0')
    report = {'status':'running','real_uploads':0,'real_logins':0,
              'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
              'scope':'production Operation/parsers/codec/SD writers/EPUB packager; synthetic loopback transport; no hardware or real cloud write proof'}
    started = time.monotonic()
    try:
        with (args.out/'stdout.log').open('w') as stdout, (args.out/'stderr.log').open('w') as stderr:
            result = subprocess.run([str(binary)],cwd=args.root,env=env,stdout=stdout,stderr=stderr,timeout=45)
        if result.returncode:
            raise RuntimeError(f'host pipeline exited {result.returncode}; inspect logs')
        logs=(args.out/'stdout.log').read_text(errors='replace')+(args.out/'stderr.log').read_text(errors='replace')
        if 'MANAGED_ACCEPTANCE_PASS' not in logs or 'chapter=0 refreshed=0' not in logs or 'chapter=1 refreshed=0' not in logs:
            raise RuntimeError('two distinct chapter contexts and completion not observed')
        epubs=list(sd.rglob('*.epub'))
        if len(epubs)!=1:
            raise RuntimeError('expected one packaged EPUB')
        with zipfile.ZipFile(epubs[0]) as archive:
            if archive.testzip() is not None:
                raise RuntimeError('EPUB CRC failed')
            opf=ET.fromstring(archive.read('OEBPS/content.opf'))
            ns={'o':'http://www.idpf.org/2007/opf'}
            items={item.attrib['id']:item.attrib['href'] for item in opf.findall('o:manifest/o:item',ns)}
            chapters=[archive.read('OEBPS/'+items[item.attrib['idref']]) for item in opf.findall('o:spine/o:itemref',ns)]
            if len(chapters)!=2 or any(b'hello' not in chapter for chapter in chapters):
                raise RuntimeError('decoded two-chapter content mismatch')
        if (sd/'.crosspoint/weread/session.bin').exists():
            raise RuntimeError('managed fixture unexpectedly stored a web credential')
        report.update(status='passed', chapters=2, epub_sha256=hashlib.sha256(epubs[0].read_bytes()).hexdigest(),
                      elapsed_ms=round((time.monotonic()-started)*1000))
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (args.out/'result.json').write_text(json.dumps(report,indent=2))
    print('PASS managed production pipeline:',args.out/'result.json')


if __name__ == '__main__':
    main()
