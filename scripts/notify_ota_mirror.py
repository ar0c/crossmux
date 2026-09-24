#!/usr/bin/env python3
"""Notify the fork's K3s mirror after its GitHub Release is verified."""

import argparse
import hashlib
import hmac
import json
import os
import time
import urllib.request

ENDPOINT = 'https://ooo.ar0c.com/hooks/release'


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, new_url):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', choices=('stable', 'nightly'), required=True)
    parser.add_argument('--build-id', required=True)
    parser.add_argument('--sha', required=True)
    args = parser.parse_args()

    key_hex = os.environ.get('CROSSMUX_OTA_HOOK_KEY', '')
    key = bytes.fromhex(key_hex)
    if len(key) != 32:
        raise ValueError('CROSSMUX_OTA_HOOK_KEY must contain 32 bytes')
    body = json.dumps({'channel': args.channel, 'buildId': args.build_id, 'sha': args.sha},
                      sort_keys=True, separators=(',', ':')).encode()
    timestamp = str(int(time.time()))
    digest = hmac.new(key, timestamp.encode() + b'\n' + body, hashlib.sha256).hexdigest()
    request = urllib.request.Request(ENDPOINT, data=body, headers={
        'Content-Type': 'application/json',
        'User-Agent': 'crossmux-release-sync/1.0',
        'X-CrossMux-Timestamp': timestamp,
        'X-CrossMux-Signature': 'sha256=' + digest,
    }, method='POST')
    with urllib.request.build_opener(NoRedirect()).open(request, timeout=900) as response:
        result = json.load(response)
    if result.get('channel') != args.channel or result.get('buildId') != args.build_id:
        raise ValueError('mirror acknowledged another release')
    print(f"K3s mirror synchronized {args.channel} {args.build_id} ({result['assetsVerified']} assets)")


if __name__ == '__main__':
    main()
