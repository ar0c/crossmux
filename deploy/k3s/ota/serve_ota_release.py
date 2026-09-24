#!/usr/bin/env python3
"""Read-only, path-restricted HTTP server for verified CrossMux OTA files."""

import argparse
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

BUILD = re.compile(r'(?:nightly|stable)-build-[0-9a-f]{40}-[0-9]+-[0-9]+')
FILE = re.compile(r'[a-z0-9][a-z0-9._-]{0,127}')


class Handler(BaseHTTPRequestHandler):
    root = Path('/data')

    def do_GET(self):
        self.serve_file(False)

    def do_HEAD(self):
        self.serve_file(True)

    def serve_file(self, head_only):
        path = urlsplit(self.path).path
        if path == '/healthz':
            body = b'ok\n'
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            if not head_only:
                self.wfile.write(body)
            return
        parts = path.split('/')
        if len(parts) != 5 or parts[:3] != ['', 'releases', 'download']:
            self.send_error(404)
            return
        tag, name = parts[3:]
        rolling = tag in ('nightly', 'stable') and name == 'release-index.json'
        immutable = BUILD.fullmatch(tag) and FILE.fullmatch(name) and (
            name.endswith('.bin') or name.endswith('.json') or name.endswith('-SHA256SUMS')
        )
        if not (rolling or immutable):
            self.send_error(404)
            return
        file = self.root / tag / name
        if not file.is_file() or file.is_symlink():
            self.send_error(404)
            return
        size = file.stat().st_size
        self.send_response(200)
        self.send_header('Content-Type', 'application/json' if name.endswith('.json') else 'application/octet-stream')
        self.send_header('Content-Length', str(size))
        self.send_header('Cache-Control', 'no-store' if rolling else 'public, max-age=31536000, immutable')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        if head_only:
            return
        with file.open('rb') as source:
            while chunk := source.read(65536):
                self.wfile.write(chunk)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--port', type=int, default=8080)
    args = parser.parse_args()
    Handler.root = args.root
    ThreadingHTTPServer(('0.0.0.0', args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
