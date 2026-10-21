#!/usr/bin/env python3
"""USB-only CrossMux debug client. Never resets, flashes or retries mutations."""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import secrets
import struct
import time
import zlib


MARKER = 'CROSSMUX-USB-DEBUG-V1;'
SAFE_FIELDS = frozenset(('session id event reason marker version uptime_ms activity frame ready usb keep_awake '
                        'pending ram_free ram_largest psram_free psram_largest sync_available sync_active '
                        'sync_revision sync_queue sync_phase sync_issue sync_start_failure bytes width height '
                        'checksum inverted sleep_timeout_ms normal_sleep_timeout_ms').split())


class DeviceError(RuntimeError):
    pass


def device_id(port):
    identity = (port.serial_number or '').replace(':', '').lower()
    if not identity:
        raise DeviceError('USB device has no identity; refusing control')
    return hashlib.sha256(identity.encode()).hexdigest()[:16]


def devices():
    from serial.tools import list_ports
    return [p for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def save_png(path, pixels, width, height, inverted=False):
    """One-bit physical portrait screenshot, standard library only (no PIL)."""
    if width % 8 or len(pixels) != width * height // 8:
        raise DeviceError('Unsupported framebuffer geometry')
    stride = (height + 7) // 8
    rows = bytearray((stride + 1) * width)
    for y in range(height):
        for x in range(width):
            white = bool(pixels[y * (width // 8) + x // 8] & (0x80 >> (x % 8))) ^ bool(inverted)
            if white:
                target_x = height - 1 - y
                rows[x * (stride + 1) + 1 + target_x // 8] |= 0x80 >> (target_x % 8)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', height, width, 1, 0, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    Path(path).write_bytes(png)


class Client:
    def __init__(self, port, output, *, transport=None):
        if transport is None:
            import serial
            transport = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=1)
            transport.dtr = transport.rts = False
            transport.port = port
            transport.open()
        self.transport = transport
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.session = secrets.randbelow(0xFFFFFFFF) + 1
        self.sequence = 0
        self.logs = deque(maxlen=300)
        self.events = []
        self.partial = b''
        self.started = time.monotonic()
        self.connected = False

    def close(self):
        self.transport.close()

    def _record(self, deadline):
        while time.monotonic() < deadline:
            fragment = self.transport.read_until(b'\n', 8193)
            self.partial += fragment
            if len(self.partial) > 8192:
                raise DeviceError('Serial line exceeds bound')
            if not self.partial.endswith(b'\n'):
                continue
            line, self.partial = self.partial, b''
            if not line.startswith(b'DBG1 '):
                self.logs.append(line.decode('utf-8', errors='replace'))
                continue
            try:
                record = json.loads(line[5:])
            except (ValueError, UnicodeDecodeError) as exc:
                raise DeviceError('Malformed debug response') from exc
            if not isinstance(record, dict):
                raise DeviceError('Malformed debug response type')
            return {key: value for key, value in record.items() if key in SAFE_FIELDS}
        raise DeviceError('Timed out; mutation outcome may be unknown; no automatic resend')

    def command(self, verb, *args, timeout=20, screenshot=None):
        self.sequence += 1
        if self.sequence > 0xFFFFFFFF:
            raise DeviceError('Sequence exhausted; open a new session')
        request = f'DBG1 {self.session:08x} {self.sequence} {verb}'
        if args:
            request += ' ' + ' '.join(map(str, args))
        if '\n' in request or '\r' in request or len(request) >= 128:
            raise DeviceError('Invalid request')
        data = (request + '\n').encode('ascii')
        if self.transport.write(data) != len(data):
            raise DeviceError('Short command write; no automatic resend')
        deadline = time.monotonic() + timeout
        image_seen = False
        while True:
            record = self._record(deadline)
            self.events.append({'host_elapsed_ms': round((time.monotonic() - self.started) * 1000), **record})
            if record.get('session') != self.session or record.get('id') != self.sequence:
                continue
            event = record.get('event')
            if event == 'error':
                raise DeviceError(str(record.get('reason', 'device_error')))
            if event == 'image':
                length, width, height = (record.get(key) for key in ('bytes', 'width', 'height'))
                if (screenshot is None or not all(type(v) is int and v > 0 for v in (length, width, height)) or
                        width % 8 or length != width * height // 8 or length > 1024 * 1024):
                    raise DeviceError('Invalid or unexpected image')
                payload = bytearray()
                while len(payload) < length and time.monotonic() < deadline:
                    payload.extend(self.transport.read(length - len(payload)))
                if len(payload) != length or checksum(payload) != record.get('checksum'):
                    raise DeviceError('Incomplete or corrupted image; retained previous evidence')
                target = Path(screenshot)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists() or target.with_suffix('.raw').exists():
                    raise DeviceError('Screenshot already exists; use a fresh output directory')
                target.with_suffix('.raw').write_bytes(payload)
                save_png(target, payload, width, height, record.get('inverted', False))
                image_seen = True
            if event == 'done':
                if screenshot is not None and not image_seen:
                    raise DeviceError('Screenshot completion without image')
                return record

    def hello(self):
        result = self.command('HELLO')
        if result.get('marker') != MARKER or not str(result.get('version', '')).endswith('-ws397-dbg'):
            raise DeviceError('Expected dedicated Waveshare USB debug firmware')
        self.connected = True
        return result

    def wait(self, *, activity=None, timeout=20, minimum_frame=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.command('STATUS', timeout=min(3, max(.1, deadline - time.monotonic())))
            if (status.get('ready') and not status.get('pending') and
                    (activity is None or status.get('activity') == activity) and
                    (minimum_frame is None or status.get('frame', -1) >= minimum_frame)):
                return status
            time.sleep(min(.2, max(0, deadline - time.monotonic())))
        raise DeviceError(f'Condition timeout: activity={activity!r}, minimum_frame={minimum_frame!r}')

    def evidence(self, name='failure'):
        """Read-only best effort; never clears, resets, navigates or repeats a pulse."""
        result = {}
        if self.connected:
            for label, verb in (('status', 'STATUS'), ('screenshot', 'SCREEN')):
                try:
                    result[label] = self.command(verb, timeout=3,
                                                 screenshot=self.output / f'{name}.png' if verb == 'SCREEN' else None)
                except Exception as exc:
                    result[label] = {'error': str(exc)}
        return result

    def write_report(self, success, error=None, evidence=None):
        report = dict(success=success, error=error, elapsed_ms=round((time.monotonic() - self.started) * 1000),
                      events=self.events, evidence=evidence, reset_sent=False, optical_acceptance=False)
        (self.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        # Firmware logs may contain private data. Keep them local, never print/upload them.
        (self.output / 'serial-private.log').write_text(''.join(self.logs), encoding='utf-8')


def run_scenario(client, document, timeout):
    steps = document.get('steps')
    if not isinstance(steps, list) or not 1 <= len(steps) <= 100:
        raise DeviceError('Scenario requires 1..100 steps')
    deadline = time.monotonic() + timeout
    results = []
    for index, step in enumerate(steps):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise DeviceError('Scenario total timeout')
        limit = min(float(step.get('timeout', 20)), remaining)
        if not 0 < limit <= 600:
            raise DeviceError('Invalid step timeout')
        operation = step.get('action')
        if operation == 'wait':
            result = client.wait(activity=step.get('activity'), minimum_frame=step.get('minimum_frame'), timeout=limit)
        elif operation == 'status':
            result = client.command('STATUS', timeout=limit)
        elif operation == 'keep':
            value = step.get('value')
            if type(value) is not bool:
                raise DeviceError('Keep value must be boolean')
            result = client.command('KEEP', int(value), timeout=limit)
        elif operation == 'pulse':
            key, duration = step.get('key'), step.get('duration_ms', 60)
            if key not in ('UP', 'PRESS', 'DOWN', 'BOOT', 'PWR') or type(duration) is not int or not 30 <= duration <= 3000:
                raise DeviceError('Invalid pulse')
            result = client.command('PULSE', key, duration, timeout=limit)
        elif operation == 'timeout':
            value = step.get('value_ms')
            if type(value) is not int or not (value == 0 or 1000 <= value <= 600000):
                raise DeviceError('Timeout override must be 0 or 1000..600000 ms')
            result = client.command('TIMEOUT', value, timeout=limit)
        elif operation == 'screenshot':
            result = client.command('SCREEN', timeout=limit, screenshot=client.output / f'step-{index:02d}.png')
        else:
            raise DeviceError('Unsupported scenario action')
        results.append(result)
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device-id', help='USB identity digest from devices (required for a session)')
    parser.add_argument('--output', type=Path, help='New Windows-accessible evidence directory')
    parser.add_argument('--timeout', type=float, default=120, help='Scenario total timeout, 1..600 seconds')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('devices', help='List native USB devices without opening/resetting them')
    commands.add_parser('status', help='Read firmware, page, render, RAM/PSRAM and sync state')
    watch = commands.add_parser('watch', help='Collect bounded state/phase timeline without operating the UI')
    watch.add_argument('--seconds', type=float, default=30)
    timeout = commands.add_parser('sleep-timeout', help='RAM-only accelerated sleep test; 0 restores user timeout')
    timeout.add_argument('milliseconds', type=int)
    keep = commands.add_parser('keep', help='Set RAM-only USB auto-sleep inhibition')
    keep.add_argument('value', choices=('on', 'off'))
    pulse = commands.add_parser('pulse', help='Send exactly one bounded physical-role gesture')
    pulse.add_argument('key', choices=('UP', 'PRESS', 'DOWN', 'BOOT', 'PWR'))
    pulse.add_argument('--duration-ms', type=int, default=60)
    commands.add_parser('screenshot', help='Wait for render readiness and capture a checked 1-bit PNG')
    scenario = commands.add_parser('run', help='Run bounded JSON steps; no shell commands or automatic replay')
    scenario.add_argument('scenario', type=Path)
    args = parser.parse_args(argv)
    ports = devices()
    if args.command == 'devices':
        print(json.dumps([dict(port=p.device, device_id=device_id(p)) for p in ports]))
        return 0
    if not args.device_id or not args.output:
        parser.error('--device-id and --output are required')
    if not 1 <= args.timeout <= 600:
        parser.error('--timeout must be 1..600')
    selected = [p for p in ports if device_id(p) == args.device_id]
    if len(selected) != 1:
        raise DeviceError('Expected exactly one matching USB identity')
    if args.output.exists():
        raise DeviceError('Evidence directory already exists; refusing overwrite')
    client = Client(selected[0].device, args.output)
    try:
        client.hello()
        if args.command == 'status':
            result = client.command('STATUS')
        elif args.command == 'keep':
            result = client.command('KEEP', int(args.value == 'on'))
        elif args.command == 'sleep-timeout':
            if not (args.milliseconds == 0 or 1000 <= args.milliseconds <= 600000):
                raise DeviceError('Timeout must be 0 or 1000..600000 ms')
            result = client.command('TIMEOUT', args.milliseconds)
        elif args.command == 'watch':
            if not 0 < args.seconds <= 600:
                raise DeviceError('Watch duration must be 0..600 seconds')
            deadline = time.monotonic() + args.seconds
            while True:
                result = client.command('STATUS', timeout=min(3, max(.1, deadline-time.monotonic())))
                if time.monotonic() >= deadline:
                    break
                time.sleep(min(.5, deadline-time.monotonic()))
        elif args.command == 'pulse':
            if not 30 <= args.duration_ms <= 3000:
                raise DeviceError('duration must be 30..3000 ms')
            client.wait()
            result = client.command('PULSE', args.key, args.duration_ms)
        elif args.command == 'screenshot':
            client.wait()
            result = client.command('SCREEN', screenshot=args.output / 'screen.png')
        else:
            result = run_scenario(client, json.loads(args.scenario.read_text(encoding='utf-8-sig')), args.timeout)
        client.write_report(True)
        print(json.dumps(dict(success=True, result=result, report=str(args.output / 'report.json'))))
        return 0
    except Exception as exc:
        evidence = client.evidence()
        client.write_report(False, str(exc), evidence)
        print(json.dumps(dict(success=False, error=str(exc), report=str(args.output / 'report.json'))))
        return 1
    finally:
        client.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except DeviceError as exc:
        print(json.dumps(dict(success=False, error=str(exc))))
        raise SystemExit(1) from exc
