import hashlib
import hmac
import json
import threading
import time
import unittest
import urllib.error
import urllib.request
from unittest.mock import patch

import hook_ota_release as hook


class QuietHandler(hook.Handler):
    def log_message(self, *_args):
        pass


class HookTests(unittest.TestCase):
    def setUp(self):
        self.key = b'k' * 32
        self.sha = 'a' * 40
        self.build_id = f'nightly-build-{self.sha}-1-1'
        QuietHandler.key = self.key
        QuietHandler.root = None
        self.server = hook.ThreadingHTTPServer(('127.0.0.1', 0), QuietHandler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, timestamp=None, signed=True):
        body = json.dumps({'channel': 'nightly', 'buildId': self.build_id, 'sha': self.sha}).encode()
        timestamp = str(int(time.time()) if timestamp is None else timestamp)
        signature = hmac.new(self.key, timestamp.encode() + b'\n' + body, hashlib.sha256).hexdigest()
        headers = {'X-CrossMux-Timestamp': timestamp,
                   'X-CrossMux-Signature': 'sha256=' + (signature if signed else '0' * 64)}
        return urllib.request.Request(f'http://127.0.0.1:{self.server.server_port}/hooks/release',
                                      data=body, headers=headers, method='POST')

    def test_valid_signature_triggers_exact_build(self):
        with patch.object(hook, 'mirror_with_lock', return_value=(self.build_id, 8)) as mirror:
            with urllib.request.urlopen(self.request()) as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(json.load(response)['buildId'], self.build_id)
            mirror.assert_called_once_with(None, 'nightly')

    def test_bad_signature_and_stale_timestamp_never_sync(self):
        with patch.object(hook, 'mirror_with_lock') as mirror:
            for request in (self.request(signed=False), self.request(timestamp=int(time.time()) - 301)):
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    urllib.request.urlopen(request)
                self.assertEqual(caught.exception.code, 401)
            mirror.assert_not_called()

    def test_changed_rolling_release_is_not_acknowledged(self):
        newer = 'nightly-build-' + 'b' * 40 + '-2-1'
        with patch.object(hook, 'mirror_with_lock', return_value=(newer, 8)):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(self.request())
            self.assertEqual(caught.exception.code, 409)


if __name__ == '__main__':
    unittest.main()
