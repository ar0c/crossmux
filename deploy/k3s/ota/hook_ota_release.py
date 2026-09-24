#!/usr/bin/env python3
"""Accept a narrowly scoped, signed release notification and mirror it."""

import argparse
import hashlib
import hmac
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from mirror_ota_release import BUILD_TAG, TARGETS, mirror_with_lock

MAX_BODY = 1024
MAX_CLOCK_SKEW = 300


def verify_notification(key, timestamp, signature, body, now=None):
    if not isinstance(timestamp, str) or not isinstance(signature, str) or len(body) > MAX_BODY:
        return None
    try:
        sent_at = int(timestamp)
    except (TypeError, ValueError):
        return None
    if abs((time.time() if now is None else now) - sent_at) > MAX_CLOCK_SKEW:
        return None
    expected = hmac.new(key, timestamp.encode() + b'\n' + body, hashlib.sha256).hexdigest()
    if not hmac.compare_digest('sha256=' + expected, signature):
        return None
    try:
        payload = json.loads(body)
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(payload, dict):
        return None
    channel, build_id, sha = (payload.get('channel'), payload.get('buildId'), payload.get('sha'))
    if (channel not in TARGETS or not isinstance(build_id, str) or
            not BUILD_TAG.fullmatch(build_id) or not isinstance(sha, str) or
            len(sha) != 40 or any(c not in '0123456789abcdef' for c in sha) or
            not build_id.startswith(f'{channel}-build-{sha}-')):
        return None
    return channel, build_id


class Handler(BaseHTTPRequestHandler):
    root = None
    key = None

    def respond(self, status, payload):
        body = (json.dumps(payload, separators=(',', ':')) + '\n').encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/healthz':
            self.respond(200, {'ok': True})
        else:
            self.respond(404, {'error': 'not found'})

    def do_POST(self):
        if self.path != '/hooks/release':
            self.respond(404, {'error': 'not found'})
            return
        try:
            length = int(self.headers.get('Content-Length', ''))
        except ValueError:
            length = 0
        if not 0 < length <= MAX_BODY:
            self.respond(413, {'error': 'invalid body length'})
            return
        self.connection.settimeout(15)
        try:
            body = self.rfile.read(length)
        except TimeoutError:
            self.respond(408, {'error': 'body timeout'})
            return
        if len(body) != length:
            self.respond(400, {'error': 'incomplete body'})
            return
        release = verify_notification(self.key, self.headers.get('X-CrossMux-Timestamp'),
                                      self.headers.get('X-CrossMux-Signature'), body)
        if release is None:
            self.respond(401, {'error': 'invalid notification'})
            return
        channel, requested_build = release
        try:
            actual_build, assets = mirror_with_lock(self.root, channel)
        except TimeoutError:
            self.respond(503, {'error': 'mirror busy'})
            return
        except Exception as exc:
            print(f'mirror failed: {type(exc).__name__}: {exc}', flush=True)
            self.respond(502, {'error': 'mirror failed'})
            return
        if actual_build != requested_build:
            self.respond(409, {'error': 'rolling release changed during sync', 'buildId': actual_build})
            return
        self.respond(200, {'channel': channel, 'buildId': actual_build, 'assetsVerified': assets})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--key-file', type=Path, required=True)
    parser.add_argument('--port', type=int, default=8081)
    args = parser.parse_args()
    key = bytes.fromhex(args.key_file.read_text().strip())
    if len(key) != 32:
        raise ValueError('hook key must contain 32 bytes')
    Handler.root, Handler.key = args.root, key
    ThreadingHTTPServer(('0.0.0.0', args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
