"""Exercise real debug state/protocol, host transport and OTA exclusion gates."""
from collections import deque
import configparser
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import debug_build_policy as policy
import device_debug as client
import package_nightly_target as package


def compile_run(program, headers=None):
    with tempfile.TemporaryDirectory(prefix='crossmux-debug-') as folder:
        folder = Path(folder)
        for name, content in (headers or {}).items():
            path = folder / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
        source = folder / 'check.cpp'
        source.write_text(program, encoding='utf-8')
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(folder),
                        '-I', str(ROOT / 'lib/hal'), '-I', str(ROOT / 'src'), str(source), '-o', str(folder/'check.exe')],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(folder/'check.exe')], check=True, capture_output=True, text=True)


class FirmwareTests(unittest.TestCase):
    def test_real_input_and_partial_protocol(self):
        compile_run(r'''
#include <cassert>
#include <string>
#include "DebugInputState.h"
#include "debug/DebugProtocol.h"
int main() {
  DebugInputState input;
  assert(!input.start(7, 60) && !input.start(1, 29) && !input.start(1, 3001));
  assert(input.start(2, 60));
  assert(!input.start(1, 60));
  input.update(UINT32_MAX-20, true, false);
  assert(input.pressed(2) && input.held(2) && input.elapsed()==0);
  input.update(10, true, false);
  assert(!input.anyPressed() && input.held(2) && input.elapsed()==31);
  input.update(40, true, false);
  assert(!input.held(2) && input.released(2) && input.elapsed()==61);
  assert(input.result()==DebugInputState::Result::Complete);
  input.update(50, true, false);
  assert(!input.anyReleased());
  assert(input.start(6,3000)); input.update(60,true,false); input.update(1060,true,false);
  assert(input.held(6) && input.elapsed()==1000);
  input.update(1061,true,true);
  assert(input.result()==DebugInputState::Result::Cancelled && input.cancelledThisFrame());
  assert(!input.anyReleased() && !input.held(6));
  input.update(1062,true,false); assert(!input.cancelledThisFrame());
  assert(input.start(1,60)); input.update(1070,true,false); input.update(1071,false,false);
  assert(!input.anyReleased() && input.result()==DebugInputState::Result::Cancelled);
  debugbridge::LineBuffer lines; debugbridge::Command command;
  for(char c:std::string("DBG1 12345678 1 HELLO\n")) {
    if(lines.feed(c,1)) assert(debugbridge::parse(lines.line(),command));
  }
  assert(command.session==0x12345678 && command.sequence==1 && command.fields==3);
  for(int i=0;i<200;++i) assert(!lines.feed('X',2));
  assert(!lines.feed('\n',2));
  assert(!lines.feed('D',3)); assert(!lines.feed('B',600)); assert(!lines.feed('\n',600));
  assert(!debugbridge::parse("DBG1 1 -1 KEEP 1",command));
  assert(!debugbridge::parse("DBG1 1 4294967296 KEEP 1",command));
  assert(!debugbridge::parse("DBG1 1 2 PULSE PRESS 60 EXTRA",command));
}
''')

    def test_real_bridge_dedup_pending_completion_and_lock(self):
        headers = {
            'activities/Activity.h': '#pragma once\n',
            'CrossPointSettings.h': '''#pragma once
struct SettingsStub {unsigned long getSleepTimeoutMs(){return 600000;}};
inline SettingsStub SETTINGS;
''',
            'Logging.h': r'''#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
extern unsigned long tick;
inline unsigned long millis(){return tick;}
struct SerialStub {
 std::string in,out;
 int available(){return static_cast<int>(in.size());}
 int read(){if(in.empty())return -1;int c=in[0];in.erase(0,1);return c;}
 template<class... A> void printf(const char* fmt,A... args){char b[2048];int n=std::snprintf(b,sizeof(b),fmt,args...);out.append(b,n);}
 size_t write(const uint8_t* p,size_t n){out.append(reinterpret_cast<const char*>(p),n);return n;}
};
extern SerialStub logSerial;
''',
            'HalGPIO.h': r'''#pragma once
#include "Logging.h"
#include "DebugInputState.h"
struct HalGPIO {
 enum class DebugButton {RockerUp,RockerPress,RockerDown,Boot,Power};
 bool usb=true; DebugInputState input; int pulses=0;
 bool isUsbConnected() const{return usb;}
 bool debugPulse(DebugButton,uint32_t ms){bool ok=input.start(1,ms);if(ok)++pulses;return ok;}
 DebugInputState::Result debugInputResult()const{return input.result();}
};
extern HalGPIO gpio;
''',
            'HalDisplay.h': r'''#pragma once
#include <cstdint>
struct HalDisplay {static constexpr unsigned DISPLAY_WIDTH=8,DISPLAY_HEIGHT=2;
 uint8_t data[2]={0x55,0xaa}; uint32_t getBufferSize(){return 2;} uint8_t* getFrameBuffer(){return data;}
 void waitRefreshComplete(){} bool isInverted(){return false;}};
extern HalDisplay display;
''',
            'HalMemory.h': r'''#pragma once
#include <cstddef>
struct HalMemory {struct Stats{size_t freeBytes=123,largestBlockBytes=100;};
static Stats getInternalHeap(){return {};} static Stats getPsramHeap(){return {};}};
''',
            'activities/ActivityManager.h': r'''#pragma once
#include <cstdint>
struct ActivityManager {bool ready=true;uint32_t frame=1;const char* debugActivityName(){return "Home";}
bool debugReady(){return ready;}uint32_t debugFrame(){return frame;}};
extern ActivityManager activityManager;
''',
            'activities/RenderLock.h': r'''#pragma once
extern bool lockAvailable;
struct RenderLock {enum class Mode {Try};explicit RenderLock(Mode){}bool ownsLock(){return lockAvailable;}};
''',
            'activities/apps/weread/webapi/WeReadTimeSync.h': r'''#pragma once
#include <cstdint>
namespace WeReadTimeSync {struct Status{uint32_t revision=0;int queue=0,phase=0,issue=0;};
inline bool debugStatus(Status&){return false;}inline bool active(){return false;}inline int lastStartFailure(){return 0;}}
''',
        }
        compile_run(r'''
#define CROSSMUX_USB_DEBUG 1
#define FREEINK_DEVICE_WAVESHARE_EPAPER_397 1
#define CROSSPOINT_VERSION "1.6.5-test-ws397-dbg"
#include "debug/DebugBridge.cpp"
#include <cassert>
unsigned long tick=100;
SerialStub logSerial;
HalGPIO gpio;
HalDisplay display;
ActivityManager activityManager;
bool lockAvailable=true;
void send(const char* command){logSerial.in=command;debugbridge::poll();while(logSerial.available())debugbridge::poll();}
int main(){
 assert(debugbridge::preventAutoSleep());
 send("DBG1 abcd1234 1 HELLO\n");assert(logSerial.out.find("CROSSMUX-USB-DEBUG-V1;")!=std::string::npos);
 send("DBG1 abcd1234 2 KEEP 0\n"); assert(!debugbridge::preventAutoSleep());
 send("DBG1 abcd1234 2 KEEP 1\n");assert(!debugbridge::preventAutoSleep());
 send("DBG1 abcd1234 3 KEEP 1\n");assert(debugbridge::preventAutoSleep());
 gpio.usb=false;assert(!debugbridge::preventAutoSleep());gpio.usb=true;
 send("DBG1 abcd1234 4 PULSE PRESS 60\n");assert(gpio.pulses==1);
 send("DBG1 abcd1234 4 PULSE PRESS 60\n");assert(gpio.pulses==1);
 send("DBG1 abcd1234 5 PULSE PRESS 60\n");assert(gpio.pulses==1);
 gpio.input.update(tick,true,false);tick+=70;gpio.input.update(tick,true,false);
 activityManager.ready=false; debugbridge::poll();assert(debugbridge::pending==4);
 tick+=300;debugbridge::poll();assert(debugbridge::pending==4);
 activityManager.ready=true;debugbridge::poll();tick+=150;++activityManager.frame;debugbridge::poll();
 assert(debugbridge::pending==4);tick+=201;debugbridge::poll();assert(debugbridge::pending==0);
 lockAvailable=false;send("DBG1 abcd1234 6 SCREEN\n");assert(logSerial.out.find("render_busy")!=std::string::npos);
 lockAvailable=true;send("DBG1 abcd1234 7 SCREEN\n");assert(logSerial.out.find("\"event\":\"image\"")!=std::string::npos);
 send("DBG1 abcd1234 8 PULSE PRESS 60\n");gpio.input.update(tick,true,false);
 gpio.input.update(++tick,false,false);debugbridge::poll();assert(debugbridge::pending==0);
 assert(logSerial.out.find("input_cancelled")!=std::string::npos);
 send("DBG1 abcd1234 9 TIMEOUT 1000\n");assert(debugbridge::sleepTimeout(600000)==1000);
 send("DBG1 abcd1234 10 TIMEOUT 999\n");assert(debugbridge::sleepTimeout(600000)==1000);
 send("DBG1 abcd1234 11 TIMEOUT 0\n");assert(debugbridge::sleepTimeout(600000)==600000);
}
''', headers)


class BuildPolicyTests(unittest.TestCase):
    def test_all_independent_source_filters_exclude_debug(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT/'platformio.ini', encoding='utf-8')
        for section in config.sections():
            if config.has_option(section, 'build_src_filter'):
                filters = config.get(section, 'build_src_filter')
                if '+<*>' in filters:
                    self.assertIn('-<debug/>', filters, section)
        self.assertIn('pre:scripts/debug_build_policy.py', config.get('env:simulator', 'extra_scripts'))

    def test_environment_flag_and_source_boundary(self):
        policy.validate_config(policy.DEBUG_ENV, [(policy.DEBUG_DEFINE, 1)], '+<*> -<debug/> +<debug/>')
        policy.validate_config('waveshare_epaper_397_nightly', [], '+<*> -<debug/>')
        for env, flags, filters in (
                ('waveshare_epaper_397_nightly', [(policy.DEBUG_DEFINE, 1)], '+<*> -<debug/>'),
                (policy.DEBUG_ENV, [], '+<*> -<debug/> +<debug/>'),
                (policy.DEBUG_ENV, [(policy.DEBUG_DEFINE, 0)], '+<*> -<debug/> +<debug/>'),
                ('waveshare_epaper_397_nightly', [], '+<*>'),
                ('waveshare_epaper_397_nightly', [], '+<*> -<debug/> +<debug/>')):
            with self.subTest(env=env, flags=flags), self.assertRaises(ValueError):
                policy.validate_config(env, flags, filters)

    def test_release_rejects_debug_even_after_board_marker_and_chunk_boundary(self):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'firmware.bin'
            data = bytearray(b'\xe9' + bytes(100))
            data[12:14] = (9).to_bytes(2, 'little')
            data.extend(b'CROSSPOINT-BOARD-V1:waveshare_epaper_397;')
            image.write_bytes(data)
            package.verify_firmware(image, 9, 'waveshare_epaper_397')
            data.extend(bytes(65530-len(data)))
            data.extend(policy.DEBUG_MARKER)
            image.write_bytes(data)
            policy.verify_image(image, debug=True)
            with self.assertRaises(ValueError):
                package.verify_firmware(image, 9, 'waveshare_epaper_397')


class FakeSerial:
    def __init__(self, responses):
        self.responses = deque(responses)
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def read_until(self, *_):
        return self.responses.popleft() if self.responses else b''

    def read(self, length):
        return self.responses.popleft()[:length] if self.responses else b''

    def close(self):
        pass


class ClientTests(unittest.TestCase):
    def test_partial_lines_and_no_mutation_replay_on_error(self):
        with tempfile.TemporaryDirectory() as folder:
            serial = FakeSerial([b'DBG1 {"session":7,', b'"id":1,"event":"error","reason":"input_cancelled"}\n'])
            connection = client.Client('unused', folder, transport=serial)
            connection.session = 7
            with self.assertRaisesRegex(client.DeviceError, 'input_cancelled'):
                connection.command('PULSE', 'PRESS', 60)
            self.assertEqual(len(serial.writes), 1)

    def test_checked_screenshot_and_no_private_fields_in_report(self):
        with tempfile.TemporaryDirectory() as folder:
            pixels = b'\x55\xaa'
            header = dict(session=7, id=1, event='image', bytes=2, width=8, height=2, checksum=client.checksum(pixels))
            done = dict(session=7, id=1, event='done', credential='must-not-export')
            serial = FakeSerial([b'DBG1 '+json.dumps(header).encode()+b'\n', pixels,
                                 b'DBG1 '+json.dumps(done).encode()+b'\n'])
            connection = client.Client('unused', folder, transport=serial)
            connection.session = 7
            connection.command('SCREEN', screenshot=Path(folder)/'screen.png')
            png = (Path(folder)/'screen.png').read_bytes()
            self.assertEqual(struct.unpack('>II', png[16:24]), (2, 8))
            self.assertNotIn('credential', connection.events[-1])
            connection.write_report(True)
            self.assertNotIn('must-not-export', (Path(folder)/'report.json').read_text())

    def test_corrupt_screenshot_is_rejected_without_overwriting(self):
        with tempfile.TemporaryDirectory() as folder:
            header = dict(session=7, id=1, event='image', bytes=2, width=8, height=2, checksum=1)
            serial = FakeSerial([b'DBG1 '+json.dumps(header).encode()+b'\n', b'\x55\xaa'])
            connection = client.Client('unused', folder, transport=serial)
            connection.session = 7
            target = Path(folder)/'screen.png'
            target.write_bytes(b'previous')
            with self.assertRaisesRegex(client.DeviceError, 'corrupted'):
                connection.command('SCREEN', screenshot=target)
            self.assertEqual(target.read_bytes(), b'previous')

    def test_condition_wait_and_invalid_scenarios(self):
        connection = mock.Mock()
        for document in ({'steps': []}, {'steps': [{'action': 'shell', 'command': 'unsafe'}]},
                         {'steps': [{'action': 'pulse', 'key': 'PRESS', 'duration_ms': 3001}]},
                         {'steps': [{'action': 'keep', 'value': 'on'}]}):
            with self.assertRaises(client.DeviceError):
                client.run_scenario(connection, document, 10)
        connection.wait.return_value = {'ready': 1, 'activity': 'Home'}
        client.run_scenario(connection, {'steps': [{'action': 'wait', 'activity': 'Home'}]}, 10)
        connection.wait.assert_called_once()
        connection.command.assert_not_called()


if __name__ == '__main__':
    unittest.main()
