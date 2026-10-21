"""Upload a new local BLE test book and verify its complete downloaded bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.parse
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--url', required=True, help='Reader LAN HTTP base URL, from its screen')
parser.add_argument('--file', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.url.rstrip('/')
with urllib.request.urlopen(root+'/api/status',timeout=20) as response:
    status = json.load(response)
if not str(status.get('version','')).endswith('-ws397-dbg'):
    raise SystemExit('Expected dedicated Waveshare debug firmware')
with urllib.request.urlopen(root+'/api/files?path=/',timeout=20) as response:
    files = json.load(response)
name = args.file.name
if not name.startswith('BLE-Loop-Test-') or args.file.suffix != '.txt':
    raise SystemExit('Only the task-owned BLE test fixture is allowed')
if any(f['name'] == name for f in files):
    raise SystemExit('Existing file: inspect readback; never overwrite or automatically retry')
data = args.file.read_bytes()
if len(data) > 100000: raise SystemExit('Fixture too large')
boundary = 'CrossMuxBLEFixture20261010'
body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
        'Content-Type: text/plain\r\n\r\n').encode() + data + f'\r\n--{boundary}--\r\n'.encode()
request = urllib.request.Request(root+'/upload?path=/',data=body,
                                headers={'Content-Type':f'multipart/form-data; boundary={boundary}'},method='POST')
with urllib.request.urlopen(request,timeout=60) as response:
    if response.status != 200: raise RuntimeError('Upload outcome uncertain; no replay')
with urllib.request.urlopen(root+'/download?path=/'+urllib.parse.quote(name),timeout=30) as response:
    returned = response.read(100001)
if returned != data: raise RuntimeError('Fixture readback differs; no replay')
result = dict(success=True,file=name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),download_verified=True)
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(result,indent=2))
print(json.dumps(result))
