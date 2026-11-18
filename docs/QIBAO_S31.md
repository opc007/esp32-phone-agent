# 七宝妹妹 · ESP32-S31-Korvo-1 刷脸门禁语音助手

把小智 v2.5.3 固件刷进 **ESP32-S31-Korvo-1** 开发板，加入人脸识别门禁 + 七宝妹妹人设：
只有录入人脸的主人（岗哥）才能语音指挥它干活，认出主人会喊名字打招呼、
像女朋友一样哄人；陌生人会被礼貌拒绝并显示不熟悉感。

> 这是与本仓库主线（ESP32-S3 直连手机）**独立**的另一块板子方案。
> S31 板配件更强（摄像头/大屏/触摸/TF 卡），适合做带形象和刷脸的桌面助手。

## 硬件

ESP32-S31-Korvo-1（乐鑫官方开发板）：

| 部件 | 规格 |
|---|---|
| SoC | ESP32-S31，双核 RISC-V 320MHz，16MB Flash + 16MB Octal PSRAM（200MHz） |
| 音频 | ES8389 codec，双麦输入 + 立体声输出 |
| 屏幕 | 800x480 RGB LCD + GT1151 电容触摸 |
| 摄像头 | OV3660 DVP（板载） |
| 存储 | microSD（SDMMC 4bit：CLK24/CMD25/D0=20/D1=21/D2=22/D3=23） |
| 其他 | WS2812、4 个 ADC 按键、USB |

## 功能

- **刷脸门禁**：摄像头抓帧 → ESP-DL 人脸检测+512 维特征提取 → 与主人特征点积比对（阈值 0.55）。
  刷脸成功解锁 10 分钟；期间所有控制指令放行。未录入人脸时降级放行（不会把自己锁在外面）。
  识别到主人喊「岗哥」并甜甜打招呼，陌生人回「你是谁呀，七宝不认识你」。
- **七宝形象**：LCD 上显示形象图 + 呼吸缩放 + 上下浮动 LVGL 动画（待机也像活的）。
- **自定义唤醒词**：`qi bao mei mei`（显示名「七宝妹妹」，MultiNet7 quant 模型）。
- **控制电脑**：复用主线的设备端 MCP 工具，走 WiFi → Mac 上 `computer_portal.py:8080`，
  应用名单动态从 Portal 端读，固件不存 App 表。11 个 `self.mac.*` 工具 + `self.qibao.face`。
- **TF 卡存储**：人脸特征优先存 `/sdcard/qibao/owner.feat`，重启不丢；卡不可用回退 NVS；
  NVS 有卡无时自动迁移到卡。启动时做读写自检。

## 编译

依赖 ESP-IDF v6.1 + 小智 v2.5.3 源码（本目录不含完整工程，只含改动文件）。
把本目录 `firmware/qibao_s31/` 下的板级源码放进小智工程的
`main/boards/espressif/esp32-s31-korvo-1/`，分区表放到 `partitions/v2/`，
`idf_component.yml` 加入人脸模型组件依赖：

```yaml
espressif/esp-dl:               { version: "3.3.11", rules: [{ if: "target in [esp32s31]" }] }
espressif/human_face_detect:    { version: "0.4.2",  rules: [{ if: "target in [esp32s31]" }] }
espressif/human_face_recognition:{ version: "0.3.2", rules: [{ if: "target in [esp32s31]" }] }
```

还需要两处工程级改动（不在板级目录）：
1. `application.cc`：STT 控制指令执行前查 `QibaoGateAllowed()`；唤醒事件调 `QibaoOnWake()` 异步刷脸。
2. `boards/common/esp_video.h`：给 `EspVideo` 加 `GetFrameData/GetFrameSize/...` getter 供人脸模块取帧。

编译脚本 `tools/build_s31_qibao.sh`（独立 `build_s31` 目录，板型锁 `KORVO_1`，
分区表 `16m_qibao.csv`，唤醒词配置见 `firmware/qibao_s31/sdkconfig.defaults.esp32s31`）。

形象图头文件 `qibao_avatar.h` 由 `tools/jpg2lvgl.py` 从图片生成（macOS `sips` 转 BMP + Python 标准库，
无需 PIL）。换形象图：

```bash
python3 tools/jpg2lvgl.py tools/qibao_assets/qibao_face480.jpg \
  <工程>/main/boards/espressif/esp32-s31-korvo-1/qibao_avatar.h qibao_avatar 360
```

## 烧录

用 `prebuilt/qibao_s31/flash.sh`（或 `tools/flash_s31_qibao.sh`）：

```bash
bash prebuilt/qibao_s31/flash.sh /dev/cu.usbserial-2110 460800
```

分区布局（`16m_qibao.csv`）：app 分区 `ota_0` 7.875MB（含人脸模型），assets 8MB。
**刻意不写 NVS 分区**，保留配网信息和人脸特征。

## 实测验证状态（2026-11-18）

固件已编译、烧录、抓完整串口启动日志验证通过：

- 正常从 `SPI_FAST_FLASH_BOOT` 引导，**无重启/无 abort**（此前板型错误导致 ES8311 写 I2C 失败无限重启，已修）
- `SKU=esp32-s31-korvo-1`，`Es8389AudioCodec initialized`
- `QibaoSD: TF 卡挂载成功：USD00（400 kHz, 4-bit）` + `读写自检：通过（写 11 / 读 11 字节）`
- `七宝人脸模块就绪，特征维度 512，已录入=0`，`self.qibao.face` 工具已注册
- `QibaoDisplay: 七宝形象图已显示（360x360）+ 呼吸/浮动动画`
- 全局 23 个 MCP 工具注册，11 个 `self.mac.*` 控制工具
- 固件 7.5MB（含 1.5MB 人脸模型），app 分区剩 4%

## 待办 / 需用户本人操作

1. **配网**：设备开机开热点 `Xiaozhi-XXXX`，手机连上访问 `http://192.168.4.1` 选家里 WiFi。
   连网后语音引擎才完整启动，"七宝妹妹"唤醒词才能实测。
2. **录入主人人脸**：目前 `已录入=0`，需通过 `self.qibao.face` 工具的 enroll 动作录入岗哥人脸
   （3 帧一致性校验）。录入后刷脸门禁才真正生效。
3. 形象图更多互动表情（说话高亮、开心/生气切换）可在 `qibao_display.h` 的 `SetupUI()` 基础上扩展。

## 关键踩坑（改动时留意）

- **板型必须锁死** `CONFIG_BOARD_TYPE_ESP32_S31_KORVO_1=y`，否则 Kconfig 默认选 FUNCTION_COREBOARD_1
  （ES8311），启动写 I2C 失败 abort 无限重启。
- 固件含人脸模型达 7.5MB，标准 v2/16m.csv 只给 app 4MB 塞不下 → 必须用 `16m_qibao.csv`。
- ESP-IDF 禁 RTTI，`EspVideo*` 转换用 `static_cast` 不用 `dynamic_cast`。
- TF 卡不要操作 GPIO39（那不是电源控制脚，拉高会导致 SDMMC 超时）；用探测频率 400kHz 初始化。
- 抓串口启动日志：macOS 无 `timeout`；RTS/DTR 组合会把芯片拉进下载模式。
  正确做法是先开 pyserial 串口，再子进程跑 `esptool --after hard_reset chip-id`，从 ROM 开始抓。
