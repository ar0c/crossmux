"""Compile the production gravity tracker and sensor HAL with a deterministic IMU."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def compile_run(program, waveshare=True):
    with tempfile.TemporaryDirectory(prefix="crossmux-rotation-") as directory:
        temp = Path(directory)
        (temp / "Arduino.h").write_text("""
#pragma once
#include <cmath>
#include <cstdint>
inline uint32_t clockMs = 0;
inline unsigned long millis() { return clockMs; }
""")
        (temp / "BoardConfig.h").write_text("""
#pragma once
namespace BoardConfig {
enum class ImuType { None, Qmi8658, Sc7a20h };
struct Sensors { ImuType imuType; };
struct Board { Sensors sensors; };
inline constexpr Board ACTIVE{{ImuType::Qmi8658}};
}
""")
        (temp / "Logging.h").write_text("""
#pragma once
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
""")
        (temp / "Imu.h").write_text("""
#pragma once
class Imu {
 public:
  struct Sample { float ax, ay, az, gx, gy, gz; };
  inline static Sample sample{0, 1, 0, 0, 0, 0};
  inline static int reads = 0, sleeps = 0, wakes = 0;
  inline static bool available = true, readable = true;
  bool begin() { return available; }
  bool hasGyroscope() const { return true; }
  bool sleep() { ++sleeps; return true; }
  bool wake() { ++wakes; return true; }
  bool read(Sample& out) { ++reads; out = sample; return readable; }
};
""")
        (temp / "CrossPointSettings.h").write_text("""
#pragma once
#include <cstdint>
struct CrossPointSettings {
  static constexpr uint8_t ORIENTATION_COUNT = 4;
  uint8_t orientation = 0, readerAutoRotate = 0;
};
inline CrossPointSettings testSettings;
#define SETTINGS testSettings
""")
        (temp / "I18n.h").write_text("""
#pragma once
enum class StrId { STR_PORTRAIT, STR_LANDSCAPE_CW, STR_ORIENTATION_INVERTED,
                   STR_LANDSCAPE_CCW, STR_ORIENTATION_AUTO };
""")
        source = temp / "test.cpp"
        source.write_text(program)
        exe = temp / "test.exe"
        subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++20", "-Wall", "-Wextra", "-Werror",
            f"-DFREEINK_DEVICE_WAVESHARE_EPAPER_397={int(waveshare)}", "-DFREEINK_DEVICE_READPICO=0",
            f"-I{temp}", f"-I{ROOT / 'lib/hal'}", f"-I{ROOT / 'src'}", str(source),
            str(ROOT / "lib/hal/HalTiltSensor.cpp"), "-o", str(exe)], check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], check=True, capture_output=True, text=True)


class AutoRotationTests(unittest.TestCase):
    def test_readable_partial_chapter_can_rotate_but_active_work_cannot(self):
        source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        body = source[source.index("bool EpubReaderActivity::allowsReaderAutoRotation() const"):].split("\n}\n",1)[0] + "\n}\n"
        program = r"""#include <cassert>
#include <atomic>
#include <cstdint>
#include <Arduino.h>
#define ENABLE_CHINESE_VERSION 1
struct RenderLock {
 enum class Mode {Try}; inline static bool available=true;
 explicit RenderLock(Mode) {} bool ownsLock() const {return available;}
};
struct WeReadTimeSync {inline static bool running=false;static bool active(){return running;}};
struct Section {bool building=true;bool isBuilding() const {return building;}};
struct Popup {bool active=false;bool isActive()const{return active;}};
struct EpubReaderActivity {
 enum class Overlay {None,Menu}; enum class FontPromptState {Idle,Waiting};
 bool epub=true; Section* section; std::atomic<bool> pageRendered{true};
 Overlay overlay=Overlay::None; Popup overlayPopup,loadFailurePopup,fontPreview;
 FontPromptState fontPromptState=FontPromptState::Idle;
 bool skipNextButtonCheck=false; int pendingManualTurn=0;
 uint32_t lastRenderCompleteMs=500,lastPageTurnTime=0;
 bool allowsReaderAutoRotation()const;
};
""".replace('Popup overlayPopup,loadFailurePopup,fontPreview;','Popup overlayPopup,loadFailurePopup; struct Preview {bool enabled=false;bool active()const{return enabled;}} fontPreview;')+body+r"""
int main() {
 Section section;
 clockMs=1000;
 EpubReaderActivity reader; reader.section=&section;
 // A readable page may coexist with a paused incremental chapter builder.
 assert(reader.allowsReaderAutoRotation());
 RenderLock::available=false;assert(!reader.allowsReaderAutoRotation());RenderLock::available=true;
 reader.pageRendered=false;assert(!reader.allowsReaderAutoRotation());reader.pageRendered=true;
 reader.overlay=EpubReaderActivity::Overlay::Menu;assert(!reader.allowsReaderAutoRotation());reader.overlay=EpubReaderActivity::Overlay::None;
 reader.skipNextButtonCheck=true;assert(!reader.allowsReaderAutoRotation());reader.skipNextButtonCheck=false;
 WeReadTimeSync::running=true;assert(!reader.allowsReaderAutoRotation());WeReadTimeSync::running=false;
 reader.pendingManualTurn=1;assert(!reader.allowsReaderAutoRotation());reader.pendingManualTurn=0;
 clockMs=700;assert(!reader.allowsReaderAutoRotation());clockMs=1000;
 assert(reader.allowsReaderAutoRotation());
}
"""
        compile_run(program)

    def test_waveshare_bsp_normalizes_physical_portrait(self):
        # Compile the pinned SDK, real maintained board profile and production
        # classifier together; mocking BoardConfig would hide a mount mistake.
        sdk = ROOT / "freeink-sdk/libs/hardware"
        with tempfile.TemporaryDirectory(prefix="crossmux-rotation-bsp-") as directory:
            temp = Path(directory)
            (temp / "soc").mkdir()
            (temp / "soc/soc_caps.h").write_text("#pragma once\n#define SOC_I2C_NUM 2\n")
            (temp / "Wire.h").write_text(
                f'#pragma once\n#include "{sdk.as_posix()}/Imu/test/host/stubs/Wire.h"\n'
                'inline TwoWire Wire1;\n')
            source = temp / "test.cpp"
            source.write_text(r'''
#include <cassert>
#include "BoardConfig.h"
#include "Imu.h"
#include "Wire.h"
#include "OrientationTracker.h"
int main() {
  assert(BoardConfig::ACTIVE.board == BoardConfig::Board::WaveshareEpaper397);
  Wire.devices[0x6B] = true;
  Wire.regs[0x6B][0] = 5;
  Imu imu;
  assert(imu.begin());
  // Real upright observation: raw +X carries gravity, not raw +Y.
  Wire.regs[0x6B][0x36] = 0x40;
  Imu::Sample sample{};
  assert(imu.read(sample));
  assert(sample.ax == 0 && sample.ay == 1 && sample.az == 0);
  OrientationTracker tracker;
  uint8_t out = 255;
  for (uint32_t t = 0; t < 900; t += 100)
    assert(!tracker.update(sample.ax,sample.ay,sample.az,t,2,out));
  assert(tracker.update(sample.ax,sample.ay,sample.az,900,2,out));
  assert(out == CrossPointOrientation::PORTRAIT);
}
''')
            exe = temp / "test.exe"
            subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-DFREEINK_DEVICE_WAVESHARE_EPAPER_397=1",
                f"-I{temp}",
                f"-I{sdk / 'Imu/test/host/stubs'}",
                f"-I{sdk / 'InputManager/test/host/metalio_stubs'}",
                f"-I{sdk / 'BoardConfig/include'}", f"-I{sdk / 'Imu/include'}",
                f"-I{ROOT / 'lib/hal'}", str(source), str(sdk / "Imu/src/Imu.cpp"),
                "-o", str(exe)], check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)

    def test_posture_uses_stable_gravity_when_gyro_is_saturated(self):
        compile_run(r'''
#include <cassert>
#include "HalTiltSensor.h"
int main() {
  HalTiltSensor sensor;
  sensor.begin(); sensor.update(0,0,true,true);
  // Captured on Waveshare: acceleration stays live while all three gyro
  // outputs stay at the negative rail. Posture must not depend on that gyro.
  Imu::sample = {-1,0,0,-512,-512,-512};
  uint8_t out = 255;
  for (clockMs = 300; clockMs < 1200; clockMs += 100) {
    sensor.update(0,0,true,true); assert(!sensor.takeOrientation(out));
  }
  sensor.update(0,0,true,true);
  assert(sensor.takeOrientation(out) && out == CrossPointOrientation::LANDSCAPE_CW);
  Imu::sample = {0,1,0,-512,-512,-512};
  for (clockMs = 1300; clockMs < 2200; clockMs += 100) {
    sensor.update(0,1,true,true); assert(!sensor.takeOrientation(out));
  }
  sensor.update(0,1,true,true);
  assert(sensor.takeOrientation(out) && out == CrossPointOrientation::PORTRAIT);
  assert(!sensor.wasTiltedForward() && !sensor.wasTiltedBack());
}
''')

    def test_continuously_moving_gravity_restarts_dwell(self):
        compile_run(r'''
#include <cassert>
#include <cmath>
#include "OrientationTracker.h"
int main() {
  OrientationTracker tracker;
  uint8_t out = 255;
  // Still classified as landscape throughout, but changing by 12 degrees
  // every 100 ms must never satisfy a stable interval.
  for (uint32_t t = 0; t < 3000; t += 100) {
    const float a = (t / 100 % 4) * 12 * 3.14159265358979323846f / 180;
    assert(!tracker.update(-std::cos(a),0,std::sin(a),t,0,out));
  }
  for (uint32_t t = 3000; t < 3900; t += 100)
    assert(!tracker.update(-1,0,0,t,0,out));
  assert(tracker.update(-1,0,0,3900,0,out));
}
''')

    def test_clockwise_physical_rotation_and_portrait_return(self):
        compile_run(r'''
#include <cassert>
#include <cmath>
#include "OrientationTracker.h"
int main() {
  OrientationTracker tracker;
  uint8_t current = CrossPointOrientation::PORTRAIT, out = 255;
  uint32_t now = 100;
  // Specific force points up. When the board rotates clockwise, its right
  // axis points down, so the force has a negative X component in board space.
  const float angles[] = {90, 0, -90, 0, 180, 0};
  const uint8_t expected[] = {1, 0, 3, 0, 2, 0};
  for (unsigned i = 0; i < 6; ++i) {
    const float radians = angles[i] * 3.14159265358979323846f / 180;
    const float x = -std::sin(radians), y = std::cos(radians);
    for (unsigned dt = 0; dt < 900; dt += 100)
      assert(!tracker.update(x,y,0,now+dt,current,out));
    assert(tracker.update(x,y,0,now+900,current,out));
    assert(out == expected[i]);
    current = out;
    now += 1000;
  }
}
''')

    def test_rotation_roundtrip_does_not_round_down_again(self):
        compile_run(r'''
#include <cassert>
#include <algorithm>
#include <iterator>
#include "ReaderRotationAnchor.h"
int main() {
  ReaderRotationAnchor anchor;
  const uint32_t portrait[] = {0,100,200,300,400,500,600,700,800,900,1000};
  const uint32_t landscape[] = {0,170,340,510,680,850,1020};
  const auto pageStart = [](const auto& pages, uint32_t offset) {
    return *std::prev(std::upper_bound(std::begin(pages), std::end(pages), offset));
  };
  uint32_t current = 900;
  for (int i=0; i<8; ++i) {
    current = pageStart(landscape, *anchor.capture(0,current));
    assert(current == 850);
    current = pageStart(portrait, *anchor.capture(0,current));
    assert(current == 900);
  }
  // Navigation starts a new reading position, including within the same spine.
  anchor.reset();
  assert(anchor.capture(0,700) == 700);
  assert(anchor.capture(1,200) == 200);
  assert(!anchor.capture(1,std::nullopt));
  assert(anchor.capture(1,0) == 0);
  static_assert(sizeof(ReaderRotationAnchor) <= 16);
}
''')

    def test_setting_selection_preserves_manual_fallback(self):
        compile_run(r'''
#include <cassert>
#include "ReaderOrientationSettings.h"
int main() {
  using namespace ReaderOrientationSettings;
  assert(labels().size() == 4 && selected() == 0);
  halTiltSensor.begin();
  assert(labels().size() == 5);
  SETTINGS.orientation = 2;
  assert(select(AUTO) && selected() == AUTO && SETTINGS.orientation == 2);
  assert(!select(AUTO));
  assert(select(3) && selected() == 3 && SETTINGS.readerAutoRotate == 0);
  assert(!select(255) && selected() == 3);
  assert(select(AUTO));
  Imu::available = false;
  halTiltSensor.begin();
  assert(labels().size() == 4 && selected() == 3);
  assert(!select(AUTO) && SETTINGS.orientation == 3);
  assert(select(1) && selected() == 1 && SETTINGS.readerAutoRotate == 0);
}
''')

    def test_gravity_dwell_ambiguity_failure_and_wrap(self):
        compile_run(r'''
#include <cassert>
#include <limits>
#include "OrientationTracker.h"
int main() {
  OrientationTracker tracker;
  uint8_t out = 255;
  const float poses[4][3] = {{0,1,0}, {-1,0,0}, {0,-1,0}, {1,0,0}};
  for (uint8_t direction = 0; direction < 4; ++direction) {
    const auto& p = poses[direction];
    tracker.reset();
    for (uint32_t t = 0; t < 900; t += 100)
      assert(!tracker.update(p[0],p[1],p[2],t,(direction+1)%4,out));
    assert(tracker.update(p[0],p[1],p[2],900,(direction+1)%4,out) && out == direction);
    assert(!tracker.update(p[0],p[1],p[2],1000,direction,out));
  }
  const float invalid[][3] = {{0,0,1}, {0.7f,0.7f,0}, {0,0,0}, {2,0,0},
                            {std::numeric_limits<float>::quiet_NaN(),0,0},
                            {std::numeric_limits<float>::infinity(),0,0}};
  for (const auto& p : invalid)
    for (uint32_t t = 0; t < 2000; t += 100) assert(!tracker.update(p[0],p[1],p[2],t,0,out));
  tracker.reset();
  assert(!tracker.update(1,0,0,0,0,out));
  assert(!tracker.update(1,0,0,1000,0,out)); // A stale sample cannot satisfy dwell.
  for (uint32_t t = 1100; t < 1900; t += 100) assert(!tracker.update(1,0,0,t,0,out));
  assert(tracker.update(1,0,0,1900,0,out));
  tracker.reset();
  const uint32_t start = UINT32_MAX - 500;
  for (uint32_t dt = 0; dt < 900; dt += 100) assert(!tracker.update(-1,0,0,start+dt,0,out));
  assert(tracker.update(-1,0,0,start+900,0,out) && out == 1);
  tracker.reset();
  for (uint32_t t = 0; t < 2000; t += 100)
    assert(!tracker.update(t % 200 ? 1 : -1,0,0,t,0,out));
}
''')

    def test_sensor_lifecycle_single_sample_and_no_flick(self):
        compile_run(r'''
#include <cassert>
#include "HalTiltSensor.h"
int main() {
  HalTiltSensor sensor;
  sensor.begin();
  assert(sensor.supportsAutoRotation() && Imu::sleeps == 1);
  sensor.update(0,0,false,false);
  assert(Imu::wakes == 0 && Imu::reads == 0);
  sensor.update(0,0,true,true);
  assert(Imu::wakes == 1);
  Imu::sample = {-1,0,0,0,0,0};
  for (clockMs = 100; clockMs < 1200; clockMs += 100) sensor.update(0,0,true,true);
  uint8_t out;
  assert(!sensor.takeOrientation(out));
  sensor.update(0,0,true,true);
  assert(sensor.takeOrientation(out) && out == 1);
  assert(!sensor.takeOrientation(out) && !sensor.hadActivity());
  assert(Imu::reads == 10); // One sample per eligible poll, shared by consumers.
  Imu::sample.gx = 400;
  for (clockMs = 1300; clockMs < 3000; clockMs += 100) sensor.update(1,1,true,true);
  assert(!sensor.wasTiltedForward() && !sensor.wasTiltedBack() && !sensor.hadActivity());
  sensor.update(0,1,false,false);
  assert(Imu::sleeps == 2 && !sensor.takeOrientation(out));
  const int oldReads = Imu::reads;
  sensor.update(0,1,false,false);
  assert(Imu::reads == oldReads);
  // Existing manual flick-to-turn still works when automatic posture is off.
  sensor.update(1,0,true,false);
  clockMs += 700;
  sensor.update(1,0,true,false);
  assert(sensor.wasTiltedForward() && sensor.hadActivity());
  assert(!sensor.wasTiltedForward());
}
''')

    def test_read_failure_and_modal_reset_dwell(self):
        compile_run(r'''
#include <cassert>
#include "HalTiltSensor.h"
int main() {
  HalTiltSensor sensor;
  sensor.begin(); sensor.update(0,0,true,true);
  Imu::sample = {-1,0,0,0,0,0};
  for (clockMs = 300; clockMs <= 1000; clockMs += 100) sensor.update(0,0,true,true);
  Imu::readable = false;
  sensor.update(0,0,true,true);
  Imu::readable = true;
  uint8_t out;
  for (clockMs = 1200; clockMs < 2100; clockMs += 100) {
    sensor.update(0,0,true,true); assert(!sensor.takeOrientation(out));
  }
  sensor.update(0,0,false,false); // Popup/home/held input clears candidates.
  sensor.update(0,0,true,true);
  for (clockMs = 2400; clockMs < 3300; clockMs += 100) {
    sensor.update(0,0,true,true); assert(!sensor.takeOrientation(out));
  }
  sensor.update(0,0,true,true);
  assert(sensor.takeOrientation(out) && out == 1);
  Imu::available = false;
  HalTiltSensor missing; missing.begin();
  assert(!missing.supportsAutoRotation());
}
''')

    def test_other_hardware_with_imu_is_not_opted_in(self):
        compile_run(r'''
#include <cassert>
#include "HalTiltSensor.h"
int main() {
  HalTiltSensor sensor;
  sensor.begin();
  assert(sensor.isAvailable() && !sensor.supportsAutoRotation());
  sensor.update(0,0,true,true);
  assert(Imu::wakes == 0 && Imu::reads == 0);
}
''', waveshare=False)


if __name__ == "__main__":
    unittest.main()
