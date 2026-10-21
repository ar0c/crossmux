import http.client
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

from serve_ota_release import Handler


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        (root / tag).mkdir()
        (root / tag / 'firmware.bin').write_bytes(bytes(range(256)) * 8)
        self.path = f'/releases/download/{tag}/firmware.bin'
        handler = type('TestHandler', (Handler,), {'root': root, 'log_message': lambda *args: None})
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def request(self, method='GET', path=None, **headers):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=3)
        connection.request(method, path or self.path, headers=headers)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def test_complete_head_and_resumed_bytes_match(self):
        status, headers, body = self.request()
        self.assertEqual((status, len(body)), (200, 2048))
        self.assertEqual(headers['Content-Length'], '2048')
        first = self.request(Range='bytes=0-511')
        rest = self.request(Range='bytes=512-')
        self.assertEqual((first[0], rest[0]), (206, 206))
        self.assertEqual(first[1]['Content-Range'], 'bytes 0-511/2048')
        self.assertEqual(first[2] + rest[2], body)
        self.assertEqual(self.request(Range='bytes=-10')[2], body[-10:])
        head = self.request('HEAD')
        self.assertEqual((head[0], head[1]['Content-Length'], head[2]), (200, '2048', b''))

    def test_rejects_invalid_ranges_and_paths(self):
        for value in ['bytes=9999-', 'bytes=8-2', 'bytes=-0', 'bytes=0-1,3-4', 'items=0-1']:
            response = self.request(Range=value)
            self.assertEqual(response[0], 416)
            self.assertEqual(response[1]['Content-Range'], 'bytes */2048')
        for path in ['/private', '/releases/download/nightly/../secret', '/releases/download/nightly/%2e%2e']:
            self.assertEqual(self.request(path=path)[0], 404)
        self.assertEqual(self.request('POST')[0], 501)


if __name__ == '__main__':
    unittest.main()
