import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from remote_debug import Remote, RemoteError, MARKER


class Transport:
    def __init__(self, mac='70:04:1d:db:aa:18'):
        self.mac, self.sent, self.reply = mac, [], b''
    def write(self, data):
        self.sent.append(data)
        _, session, sequence, verb, *args = data.decode().split()
        self.reply = b'REM1 ' + json.dumps(dict(session=int(session,16), id=int(sequence), event='done',
                        marker=MARKER, model='StickS3', mac=self.mac)).encode() + b'\n'
        return len(data)
    def read_until(self, *args):
        result, self.reply = self.reply, b''
        return result
    def close(self): pass


class ClientTests(unittest.TestCase):
    def test_accessories_require_identity_and_validate_controls(self):
        transport = Transport()
        client = Remote(transport.mac, transport=transport)
        for command, args in (('JOY', ('RIGHT', 100)), ('HAPTIC', (100,))):
            with self.assertRaises(RemoteError): client.command(command, *args)
        self.assertEqual(transport.sent, [])
        client.hello()
        with self.assertRaises(RemoteError): client.joystick('DIAGONAL')
        with self.assertRaises(RemoteError): client.joystick('CLICK', 2001)
        self.assertEqual(len(transport.sent), 1)

    def test_identity_gates_mutation(self):
        transport = Transport()
        client = Remote(transport.mac, transport=transport)
        with self.assertRaises(RemoteError): client.command('BUTTON','A',80)
        self.assertEqual(transport.sent, [])
        client.hello()
        client.command('DISCONNECT')
        self.assertEqual(len(transport.sent), 2)

    def test_wrong_device_rejected(self):
        client = Remote('00:00:00:00:00:00', transport=Transport())
        with self.assertRaises(RemoteError): client.hello()
        self.assertFalse(client.verified)

    def test_fragmented_reply_is_assembled_without_resend(self):
        transport = Transport()
        def fragments(*args):
            result, transport.reply = transport.reply[:17], transport.reply[17:]
            return result
        transport.read_until = fragments
        client = Remote(transport.mac,transport=transport)
        self.assertEqual(client.hello()['marker'],MARKER)
        self.assertEqual(len(transport.sent),1)

    def test_short_write_never_replayed(self):
        transport = Transport()
        client = Remote(transport.mac, transport=transport)
        transport.write = lambda data: transport.sent.append(data) or 1
        with self.assertRaises(RemoteError): client.hello()
        self.assertEqual(len(transport.sent),1)

    def test_invalid_gesture_rejected_before_write(self):
        transport = Transport()
        client = Remote(transport.mac, transport=transport)
        with self.assertRaises(RemoteError): client.button('A',3000)
        self.assertEqual(transport.sent,[])

    def test_callback_count_is_not_report_count(self):
        client = Remote('70:04:1d:db:aa:18',transport=Transport())
        before = dict(ready=1,busy=0,issued=0,completed=0,reports_submitted=0,
                      notify_callbacks=0,notify_errors=0,dropped=0)
        after = dict(ready=1,busy=0,issued=1,completed=1,reports_submitted=2,
                     notify_callbacks=4,notify_errors=0,dropped=0)
        replies = iter((before,dict(busy=1),after))
        client.command = lambda *args: next(replies)
        self.assertEqual(client.button('B',80),after)

if __name__ == '__main__': unittest.main()
