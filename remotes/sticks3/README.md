# StickS3 CrossMux 蓝牙遥控器

独立的 M5Stack StickS3 固件，通过 BLE HID 键盘控制 CrossMux 阅读器，并提供 USB 命令接口和双设备闭环测试工具。阅读器仍使用自己的 Waveshare 3.97 固件；本项目位于 CrossMux 仓库的 `remotes/sticks3/`，作为独立构建目标；不加入阅读器 Nightly / OTA 目标表。

## 按键与连接

| StickS3 操作 | HID 键 | CrossMux 映射 |
|---|---|---|
| A 短按 | PageDown (`0x4e`) | 下一页 |
| B 短按 | PageUp (`0x4b`) | 上一页 |
| A 长按，至少 650 ms | Escape (`0x29`) | 返回 |
| B 长按，至少 650 ms | Enter (`0x28`) | 确认／阅读菜单 |
| Joystick2 四向拨动 | 方向键 | 首页、书库、菜单、设置的方向导航；阅读页左右翻页 |
| Joystick2 短按／长按 ≥650 ms | Enter／Escape | 确认／返回 |

0.2.0 新增 Unit Joystick2 和震动 HAT，沿用已确认的 SDA9/SCL10、I²C 0x63、马达 G1 接线。禁止将马达控制接回启动绑带 G0。摇杆拨动一次执行一次，回中后才能再次拨动；上电、重连和按压后都要求回中，持续拨住不会跨页面重复操作。

0.2.3 按当前持握朝向的四向原始采样校准：上 `(127,3)`、下 `(-126,2)`、左 `(0,-127)`、右 `(0,127)`。原始轴到逻辑轴为 `(rawY, -rawX)`，`joy_axis_mode=swap_xy_invert_y`；`STATUS` 的轴值仍为硬件原始采样，USB `JOY` 始终使用逻辑方向。0.2.2 的固定旋转仍让实体上／下输出右／左，已由这组采样的回归测试复现并修正。换安装朝向后应重新采集方向，不能沿用旧朝向结论。

全局摇杆需要更新后的 CrossMux 接收端：微雪的清醒页面保持蓝牙，标准方向键无需额外学习，用户已有映射优先。Wi-Fi、独占存储和深睡眠仍按阅读器原资源策略处理。A/B 四项映射与 0.1.4 相同。

发出 HID 按键后提供 35 ms 本地震动反馈；它表示遥控器提交动作，接收端成功仍以阅读器证据为准。共享扩展 5V 为摇杆保持供电，续航未实测。

长按只触发一次，松开时不会再触发短按。电源键保持设备原有硬件功能。屏幕显示连接、电量和最近动作；没有开启 Wi-Fi、麦克风、扬声器或 IMU。

从 0.1.4 开始，A／B 短按交换为下一页／上一页；长按保持原功能。升级原 0.1.3 时，阅读器原 HID 映射可继续使用，无需重新配对或学习映射。

阅读器：设置 → 操作 → 蓝牙 → 开启 → 扫描并配对 → **StickS3 CrossMux**。然后进入“映射遥控器按键”，依次选择上述四项并按对应遥控器键。映射和配对记录会保留。

更新后的微雪 CrossMux 在清醒的首页、书库、设置与阅读页面自动启动蓝牙；首次排版可能推迟新连接，独占文件传输、Wi-Fi 服务和深睡眠会停止蓝牙。遥控器不能唤醒已经深睡眠的微雪；需要微雪本机电源键。A/B 提供翻页、确认和返回，摇杆提供全局方向导航。

硬件依据：[M5Stack StickS3 官方资料](https://docs.m5stack.com/en/core/StickS3)。蓝牙实现使用 Arduino-ESP32 3.3.7 自带的 NimBLE 兼容 BLE API、标准 8 字节键盘报告、Report ID 1、加密连接及可删除的 NVS 配对记录。

## USB 控制

安装 Python 的 `pyserial==3.5`。USB 使用 ESP32-S3 原生 Serial/JTAG CDC，不等待电脑连接，断开 USB 后仍可作为蓝牙遥控器工作。

```powershell
python tools/remote_debug.py --mac <StickS3的USB-MAC> status
python tools/remote_debug.py --mac <StickS3的USB-MAC> --output artifacts/next.json button A --duration-ms 80
python tools/remote_debug.py --mac <StickS3的USB-MAC> button A --duration-ms 800
python tools/remote_debug.py --mac <StickS3的USB-MAC> disconnect
python tools/remote_debug.py --mac <StickS3的USB-MAC> joystick RIGHT
python tools/remote_debug.py --mac <StickS3的USB-MAC> joystick CLICK --duration-ms 800
python tools/remote_debug.py --mac <StickS3的USB-MAC> haptic --duration-ms 200
```

`BUTTON` 注入 A/B 的按下时间，经过与实体按键相同的手势判定，再发送真实 BLE 报告。允许 30–2000 ms；只有连接加密、订阅完成且没有正在执行的动作时才接受。

客户端先核对 USB 身份、`STICKS3-REMOTE-USB-V1` 标记、运行型号和 MAC。协议为 `REM1 <session-hex> <sequence-decimal> <command> [args]`；固件拒绝当前会话内重复或倒退的序号。命令超时或短写意味着结果未知，客户端不会自动重发；先读状态和接收端证据。

关键状态：

- `ready`：遥控器端已连接、加密、订阅；接收端仍需完成 HID 初始化。
- `issued` / `completed`：手势发送和释放计数。
- `reports_submitted`：提交给 BLE 库的报告数，正常一次动作是按下和释放共两份。
- `notify_callbacks`：库回调数。Arduino 3.3.7 的提交和发送完成均可能回调，不能当作报告数量或接收 ACK。
- `notify_errors` / `dropped`：错误和丢弃动作数。
- `joy_online` / `joy_x` / `joy_y` / `joy_pressed`：模块在线与实体采样值。
- `joy_axis_mode`：实体轴到逻辑方向的转换方式。
- `joy_reads` / `joy_failures` / `joy_events`：硬件读取、失败和手势事件计数。
- `haptic_ready` / `haptic_active` / `haptic_pulses`：PWM 初始化、当前输出及脉冲计数；不是接收端 ACK。

USB 响应使用固定 1024 字节格式化缓冲，以及 ESP-IDF USB Serial/JTAG 驱动一次分配的 2 KiB TX／256 B RX 队列。实测 Arduino HWCDC 在扩大队列后仍偶发截断，因此关闭 Arduino 的 CDC 接管，由 [ESP-IDF 驱动](https://github.com/espressif/esp-idf/blob/v5.5.2/components/esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h)独占 USB。读写等待参数为零，主机停读不能拖延按键释放。BLE 库的 GATT 对象只在初始化时分配；应用没有动态动作队列。

## 双设备闭环

接收端需要 CrossMux 专用 `-ws397-dbg` 固件及其 `scripts/device_debug.py`。`--repo` 可指定实际 CrossMux 源目录，默认根据工具文件位置定位本仓库根目录，与启动时的工作目录无关。

1. 在阅读器的“映射遥控器按键”页面选中第一行，运行 `tools/learn_map.py --mac <MAC> --output <新证据目录>`。这会通过真实 BLE 学习并保存四项映射。
2. 打开单独的 `BLE-Loop-Test-*.txt` 测试书，完成排版，并停在有上一页、下一页的中间页。
3. 运行 `python tools/closed_loop.py --mac <MAC> --output <新证据目录>`。

工具验证下一页、上一页、菜单、返回，以及主动断连后的自动重连和再次翻页。每个动作只发送一次；通过接收端状态、完整帧校验和正文区域哈希判断效果。重连后还会等待 CrossMux 自身的连接完成日志。正文哈希排除动态页脚时钟，返回动作必须恢复原测试页。

证据包含 `acceptance.json`、双方协议事件、PNG／原始帧及本地串口日志。目录不可覆盖。软件帧证据不等于墨水屏光学效果或手指按键验收。原始串口日志与 Flash 备份可能有私密数据，只保存在本机，不加入发布包。

辅助工具：`reader_control.py` 执行有界的阅读器 USB 场景；`upload_fixture.py` 只创建不存在的任务测试 TXT，并完整下载回读校验。文件传输进入／退出可能让阅读器重启，应重新获取串口和 `HELLO`，不能重发结果未知的按键。

`observe_joystick.py` 默认采集实体右、左各一次；`--directions LEFT RIGHT RIGHT LEFT` 可指定 2–8 次左右平衡序列，每次回中并至少停两秒。仅在远离首末页的本地测试书使用；采集顺序必须与实际操作一致，额外动作会停止验证并保留证据。

`calibrate_joystick.py --directions UP --verify` 可只读核验实体向上是否输出 UP；省略参数仍按上、下、左、右采集。工具不会修改模块校准；`--verify` 要求观察方向与指定方向一致。

`python tools/usb_stress.py --mac <MAC> --output <新证据目录>` 进行只读传输回归：默认 20 轮，每轮空闲 1 秒，再交替读取遥控器状态及阅读器校验帧，共 100 条状态、20 帧。遇到首次异常即停止，不发送按键。

## 烧录工具直接下载完整固件

本机完整文件为 `firmware/StickS3-CrossMux-Remote-0.2.3-full-private.bin`，大小 **8,388,608 字节（8 MiB）**。在支持 ESP32-S3 的烧录工具中选择这个文件并下载，无需运行 Python 或重新编译：

| 参数 | 值 |
|---|---|
| 芯片 | ESP32-S3 |
| 文件地址 | `0x0` |
| Flash 模式 | DIO |
| Flash 频率 | 80 MHz |
| Flash 大小 | 8 MB |

完整镜像包含原备份的启动程序、分区表、救援固件、配置和数据，reader 应用已替换为已实机安装过的 0.2.3。烧录会恢复原备份配置，可能需要重新配对。此镜像仅适用于这台已核实布局的 StickS3，含私有配置／数据，本地保存，不公开分发。文件摘要与验证边界见 `firmware/manifest.json`；已验证文件和所有分区内容，本次未进行完整镜像的实机启动复验。

## 使用安全脚本烧录应用（可选）

本机 `firmware/` 同时保留已实机安装并读回的 0.2.3 应用、原始 8 MiB Flash 校验镜像和摘要索引。该目录不加入 Git。原始镜像用于保护救援与数据分区的校验，不能作为通用整片烧录包。

在 CrossMux 根目录执行：

```powershell
# 仅检查文件摘要与 Python 工具，不连接设备
.\.platformio\penv\Scripts\python.exe remotes/sticks3/tools/flash_saved.py --check
# 连接 StickS3 后执行；只写已核实的 reader 应用
.\.platformio\penv\Scripts\python.exe remotes/sticks3/tools/flash_saved.py
```

本机保留阅读器现有的 Python 工具环境。若构建工具把该环境的 esptool 改为其他版本，可恢复已验证版本后再次检查：

```powershell
.\.platformio\penv\Scripts\uv.exe pip install --python .\.platformio\penv\Scripts\python.exe esptool==5.3.0 pyserial==3.5
```

`flash_saved.py` 先检查两个镜像的长度与 SHA-256，再调用现有安全烧录器；原有设备身份、分区、OTA、当前应用和保留区域校验全部执行。当前应用须匹配保留的 0.2.3，超时或失败不自动重试，证据输出到新建 `artifacts/flash-*`。本轮只验证了离线检查和主机测试，没有再次烧录。

## 构建、烧录与恢复

固定依赖：pioarduino Core 6.2.0、平台 55.03.37、Arduino-ESP32 3.3.7、M5Unified 0.2.17、M5GFX 0.2.32。平台与包隔离在子工程的 `.platformio/`，不会更换阅读器固定的工具链；无需设置 `USERPROFILE`。

在 CrossMux 仓库根目录构建：

```bash
python -m pip install -r remotes/sticks3/requirements.txt
pio run -d remotes/sticks3 -e sticks3_remote -j 4
```

应用产物为 `remotes/sticks3/.pio/build/sticks3_remote/firmware.bin`。独立的 [StickS3 CI](../../.github/workflows/sticks3-remote.yml) 运行主机测试、构建并保存应用及 SHA-256；不会发布或烧录。以下调试、测试和安全烧录命令均在 `remotes/sticks3/` 下执行：

```powershell
pio run -e sticks3_remote -j 4
python -m unittest discover -s tests -v
New-Item -ItemType Directory -Force artifacts | Out-Null
g++ -std=c++17 -Wall -Wextra -Werror -I include tests/gesture_test.cpp -o artifacts/gesture_test.exe
g++ -std=c++17 -Wall -Wextra -Werror -I include tests/joystick_test.cpp -o artifacts/joystick_test.exe
./artifacts/gesture_test.exe
./artifacts/joystick_test.exe
```

**普通 `upload`、`uploadfs`、`uploadfsota` 与 `erase` 目标会被构建脚本拒绝。** 本机 StickS3 现有布局含救援固件：factory/rescue `0x10000`，唯一 OTA 应用 reader `0xd0000`、容量 `0x3a0000`，SPIFFS `0x470000`。该地址不是所有 StickS3 的通用地址。

使用安装了 esptool 5.3.0 和 pyserial 的 Python 环境运行 `tools/flash_app.py --help`；先用同一解释器检查 `-m esptool version`，不要依赖系统 `python` 的环境。工具只接受本次已验证的硬件身份和分区布局，要求完整原 Flash 备份及其 SHA-256，先核对当前分区、OTA 状态和原镜像，再只写应用。升级时用 `--previous-firmware` 核对已知旧应用。它不会写 bootloader、分区表、OTA 元数据、NVS、SPIFFS、救援镜像或 eFuse；烧录后分别校验应用及保留区域。NVS 允许射频校准和配对的正常可逆更新。

阅读器升级工具 `tools/flash_reader_app.py` 要求显式提供已验证的 esptool 脚本。本次微雪使用 `%USERPROFILE%/.platformio/packages/tool-esptoolpy/esptool.py` 5.1.2 完成 16 MiB 备份与校验；5.3 的 stub 全片读取曾中断，不能将其当作已完成备份。工具只写核实的当前有效应用槽，保留另一应用及设备数据。

回退时，从原 8 MiB 备份提取 reader 分区 `[0xd0000, 0x470000)`，重新核实设备后按原偏移恢复并回读校验。不要用普通 `0x10000` 应用地址覆盖救援镜像，也不要整片擦除。原始备份已核对摘要并保留为本目录的 `firmware/StickS3-original-flash-private.bin`，不放入 Git 或发布包。

## 本次交付

本机保留的完整固件、应用固件及原始 Flash 的大小与摘要见 `firmware/manifest.json`。0.2.3 应用是此前已实机安装并读回的镜像；迁入源码的新编译镜像已随构建缓存移除。旧独立工程、历史版本、ELF、原始遥控器日志、测试产物与专用工具链缓存已按用户要求移入回收站；阅读器工程、工具链及新源码保留。回收站占用空间尚未释放。

历史验收摘要保留在固件索引与阅读器侧说明中；原始日志已清理，四向逐项实物复验仍未完成。全局导航采集使用 `tools/global_controls.py`，实体翻页只读验收使用 `tools/observe_joystick.py`，四向原始采集使用 `tools/calibrate_joystick.py`。长续航、所有阅读格式和深睡眠唤醒周期尚未验收。
