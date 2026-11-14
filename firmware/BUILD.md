# 固件：构建与应用指南

本仓库的固件基于开源项目 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT 协议）修改，
已验证基线 commit `a73e9a7e`（v2.5.x），目标板型 `bread-compact-wifi-s3cam`（ESP32-S3 N16R8 + LCD + 麦克风）。

## 目录说明

| 路径 | 内容 |
|---|---|
| `board/` | 板级源码，整目录覆盖到上游 `main/boards/bread-compact-wifi-s3cam/` |
| `patches/0001-integrate-phone-agent.patch` | 上游非板级文件的改动（`application.cc`、`protocol.cc`、`main/CMakeLists.txt`、`idf_component.yml`） |
| `sdkconfig.board` | 完整可复现的 sdkconfig（关闭蓝牙释放 SRAM 给语音链路） |
| `apply.sh` | 一键应用上述三项 |

核心新增文件：

- **`phone_bridge.cc/.h`** — 手机直连协议桥 + 引导页 HTTP 服务 + 局域网自动发现 + 中文整句解析，本项目的主要工作量所在
- **`orb_eye_apk.h`** — 19KB 手机端 App（orb-eye，MIT）以内嵌 C 数组形式打进固件 rodata，由引导页托管下载
- **`mac_control_tools.cc/.h`** — 10 个设备端 MCP 工具注册（`self.phone.control` / `self.mac.*`）+ 本地 STT 语音兜底
- **`portal_client.cc/.h`** — 轻量 HTTP 客户端（NVS 记忆目标地址）
- **`voice_selftest.cc/.h`** — 串口自测命令台（`macphone` / `phonestatus` / `macapps` ……）

早期实验性的 BLE/USB HID 文件（`phone_hid*`、`esp_hid_gap*`、`usb_descriptors.c`）默认由 CMake 选项
`PHONE_HID_ENABLE=OFF` 排除编译，保留在仓库里供参考（见 README「为什么不用蓝牙/USB 直连」）。

## 方式一：直接烧录预编译固件（最快）

```bash
pip install esptool
cd tools && ./flash_prebuilt.sh /dev/cu.usbserial-XXXX   # macOS；Linux 为 /dev/ttyUSB0
```

预编译产物在 `prebuilt/`（bootloader + 分区表 + otadata + 应用 2.7MB + 语音资源），
只写这 5 个分区，**不动 NVS**——已有的 WiFi 配网、账号绑定都不受影响。

## 方式二：从源码构建

```bash
# 1. 克隆上游并应用改动
git clone https://github.com/78/xiaozhi-esp32.git ~/esp/xiaozhi-esp32
cd firmware && ./apply.sh ~/esp/xiaozhi-esp32

# 2. 安装 ESP-IDF v6.1（一次性）
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.1
cd esp-idf-v6.1 && ./install.sh

# 3. 编译
cd ~/esp/xiaozhi-esp32
. ~/esp/esp-idf-v6.1/export.sh
idf.py -B build -DIDF_TARGET=esp32s3 -DBOARD_NAME=bread-compact-wifi-s3cam build
idf.py -B build -p /dev/cu.usbserial-XXXX flash monitor
```

编译要求：ESP-IDF v6.1（依赖 C++23，`main/CMakeLists.txt` 已固定 `-std=gnu++23`）。
固件总量约 2.7MB / 16MB flash，运行时空闲 SRAM 约 42KB。

## 烧录后第一次开机

1. 串口监视器（115200）里会打印：`手机接入引导页: http://<ESP32 IP>/`
2. 手机连上**同一个 WiFi**，浏览器打开上面的地址
3. 按页面提示：下载 19KB App → 安装 → 设置里打开无障碍开关
4. ESP32 自动发现手机（引导页来访 IP / NVS 记忆 / 同网段扫描三重保障），无需填任何 IP

## 串口自测命令（115200）

| 命令 | 作用 |
|---|---|
| `macphone <中文整句>` | 模拟一句语音，走完整解析 + 手机执行 + 回读链路 |
| `macapps` | 列出手机桌面真实安装的应用（中文名） |
| `macscreen` | 读回手机屏幕当前真实文字 |
| `phonestatus` | 手机连接状态（active / phone / apk_size） |
| `phonebind <IP>` / `phoneunbind` | 手动绑定 / 清除记住的手机并触发自动扫描 |
| `macset <IP:8080> <token>` | 切换到「语音控制电脑」老链路（可选） |
| `mactest` | 上报 10 个 MCP 工具到云端，验证 tools/list |

## 已踩过的坑（改代码前必读）

1. **`esp_http_client_open()` 只发请求头**，POST body 必须自己 `esp_http_client_write()`，
   否则 `fetch_headers()` 会死等超时（每个请求白等 6 秒）。
2. WiFi 拿到 IP 之前不能起 httpd/socket，否则 `assert tcpip_send_msg_wait_sem` 无限重启。
3. `esp_http_client_read()` 读完 `Content-Length` 仍会等连接关闭，要自己判断读够即停。
4. 这块板的 LCD 与原生 USB D-/D+ 复用 GPIO19/20，开 USB 外设会黑屏——所以数据走 WiFi。
5. 蓝牙 HID 与语音链路抢 SRAM（稳态只剩 ~7KB 会 ENOMEM），所以 BT 默认关闭。
