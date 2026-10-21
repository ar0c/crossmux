#!/usr/bin/env python3
"""Control a verified StickS3 over USB, without reset, flash or mutation retries."""
import argparse
import json
from pathlib import Path
import secrets
import time

MARKER = 'STICKS3-REMOTE-USB-V1'


class RemoteError(RuntimeError):
    pass


class Remote:
    def __init__(self, mac, *, transport=None):
        self.mac = mac.lower()
        if transport is None:
            from serial.tools import list_ports
            import serial
            ports = [p for p in list_ports.comports() if p.vid == 0x303a and p.pid == 0x1001 and
                     (p.serial_number or '').lower() == self.mac]
            if len(ports) != 1:
                raise RemoteError('Expected exactly one matching StickS3 USB identity')
            transport = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=1)
            transport.dtr = transport.rts = False
            transport.port = ports[0].device
            transport.open()
        self.transport = transport
        self.session = secrets.randbelow(0xffffffff) + 1
        self.sequence = 0
        self.events = []
        self.requests = []
        self.partial = b''
        self.verified = False

    def close(self):
        self.transport.close()

    def command(self, verb, *args, timeout=5):
        if verb not in ('HELLO', 'STATUS', 'BUTTON', 'JOY', 'HAPTIC', 'DISCONNECT'):
            raise RemoteError('Unsupported command')
        if verb not in ('HELLO', 'STATUS') and not self.verified:
            raise RemoteError('HELLO identity verification required before control')
        self.sequence += 1
        if self.sequence > 0xffffffff:
            raise RemoteError('Sequence exhausted')
        request = f'REM1 {self.session:08x} {self.sequence} {verb}'
        if args:
            request += ' ' + ' '.join(map(str, args))
        if len(request) >= 128 or '\r' in request or '\n' in request:
            raise RemoteError('Invalid request')
        data = (request + '\n').encode('ascii')
        self.requests.append(dict(id=self.sequence,verb=verb,args=args,host_monotonic=time.monotonic()))
        if self.transport.write(data) != len(data):
            raise RemoteError('Short write; outcome unknown, no automatic retry')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.partial += self.transport.read_until(b'\n', 2049)
            if len(self.partial) > 2048:
                raise RemoteError('Oversized response')
            if not self.partial.endswith(b'\n'):
                continue
            line, self.partial = self.partial, b''
            if not line.startswith(b'REM1 '):
                continue
            record = json.loads(line[5:])
            self.events.append(record)
            if record.get('session') != self.session or record.get('id') != self.sequence:
                continue
            if record.get('event') == 'error':
                raise RemoteError(record.get('reason', 'remote error'))
            if record.get('event') == 'done':
                return record
        raise RemoteError('Timeout; outcome unknown, no automatic retry')

    def hello(self):
        result = self.command('HELLO')
        if result.get('marker') != MARKER or result.get('model') != 'StickS3' or result.get('mac') != self.mac:
            raise RemoteError('Unexpected firmware or hardware identity')
        self.verified = True
        return result

    def button(self, button, duration_ms):
        return self.gesture('BUTTON', button, duration_ms)

    def joystick(self, direction, duration_ms=100):
        return self.gesture('JOY', direction, duration_ms)

    def gesture(self, verb, control, duration_ms):
        allowed = ('A', 'B') if verb == 'BUTTON' else ('UP', 'DOWN', 'LEFT', 'RIGHT', 'CLICK')
        if control not in allowed or type(duration_ms) is not int or not 30 <= duration_ms <= 2000:
            raise RemoteError('Invalid gesture or duration 30..2000 ms')
        before = self.command('STATUS')
        if not before['ready'] or before['busy']:
            raise RemoteError('Remote is not ready or is busy')
        self.command(verb, control, duration_ms)
        deadline = time.monotonic() + duration_ms / 1000 + 4
        while time.monotonic() < deadline:
            time.sleep(.08)
            after = self.command('STATUS')
            if after['busy']:
                continue
            if (after['completed'] != before['completed'] + 1 or after['issued'] != before['issued'] + 1 or
                    after['reports_submitted'] != before['reports_submitted'] + 2 or
                    after['notify_errors'] != before['notify_errors'] or after['dropped'] != before['dropped']):
                raise RemoteError('Press/release submission failed; inspect receiver, do not replay')
            return after
        raise RemoteError('Gesture completion unknown; do not replay')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mac', required=True, help='USB/base MAC, e.g. 70:04:1d:db:aa:18')
    parser.add_argument('--output', type=Path, help='Write the full command/response evidence as JSON')
    commands = parser.add_subparsers(dest='operation', required=True)
    commands.add_parser('status')
    button = commands.add_parser('button')
    button.add_argument('button', choices=('A', 'B'))
    button.add_argument('--duration-ms', type=int, default=80)
    commands.add_parser('disconnect')
    joy = commands.add_parser('joystick')
    joy.add_argument('direction', choices=('UP', 'DOWN', 'LEFT', 'RIGHT', 'CLICK'))
    joy.add_argument('--duration-ms', type=int, default=100)
    haptic = commands.add_parser('haptic')
    haptic.add_argument('--duration-ms', type=int, default=100)
    args = parser.parse_args()
    remote = None
    report = {'success': False, 'receiver_verified': False}
    try:
        remote = Remote(args.mac)
        hello = remote.hello()
        if args.operation == 'status':
            result = remote.command('STATUS')
        elif args.operation == 'button':
            result = remote.button(args.button, args.duration_ms)
        elif args.operation == 'joystick':
            result = remote.joystick(args.direction, args.duration_ms)
        elif args.operation == 'haptic':
            if not 10 <= args.duration_ms <= 250:
                raise RemoteError('Haptic duration 10..250 ms')
            result = remote.command('HAPTIC', args.duration_ms)
        else:
            result = remote.command('DISCONNECT')
        report.update(success=True, result=result)
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        if remote:
            report['events'] = remote.events
            report['requests'] = remote.requests
            report['partial_response'] = remote.partial.decode('utf-8',errors='replace')
            remote.close()
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'events'}, ensure_ascii=False))
    return 0 if report['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
