# 七宝 S31 交接（2026-11-20 固化）

给下一位开发。先复制两样东西：固件文件，和这一篇。不要用 11 月 19 日以前的交接，那里的串口、摄像头型号、激活状态都过时了。

岗哥已经看过这块板，说可以了。S31 行为以这一版为准。下一阶段是另做 ESP32-P4，给「咕咕两轮智行」高阶版智能中控屏做 AI 虚拟交互女友。不要把这份 S31 固件写进 P4。

## 固件

| 项 | 值 |
|---|---|
| 文件 | `prebuilt/qibao_s31/xiaozhi.bin`，同样字节也在 `prebuilt/qibao_s31/qibao-s31-20261120.bin` |
| 大小 | 7080416 字节 |
| SHA256 | `44029afd4b25035f8ac17a85ddf47419355fa051403ce25d0d4965ee9177fef4` |
| 标签 | `qibao-s31-20261120` |
| 固件对应代码 | `2a26dc4`（看画面那一版）。`eaadf92` 只加了固化说明，app 字节没变 |
| 仓库 | https://github.com/opc007/esp32-phone-agent |
| 分支 | `cursor/s31-voice-link-5265` |
| 拉取请求 | https://github.com/opc007/esp32-phone-agent/pull/1 |

本机路径：`/Users/ahs/Documents/esp32控制手机/prebuilt/qibao_s31/qibao-s31-20261120.bin`

只写 app 分区 `0x20000`。不要写 bootloader、分区表、`ota_data`、assets，不要擦 NVS。TF 卡上的 `/sdcard/qibao/owner.feat` 和物品模型文件保持不动。

烧录前核对 CP2102N 序列号是 `cc310f7eb6bcf011a6c7177148e9de0f`。MAC `1c:29:04:d0:58:64`。口名会在 `usbserial-110` 和 `usbserial-3130` 之间变，以序列号为准。见到 `/dev/cu.usbmodem*` 不要打开、不要复位、不要烧，那是另一块乐鑫板。

```bash
prebuilt/qibao_s31/flash.sh /dev/cu.usbserial-XXXX
```

脚本拒绝路径里带 `usbmodem` 的口。波特率 460800，烧完硬复位。控制台日志是 115200，打开串口时把 DTR 和 RTS 拉低，避免 Mac 把芯片带进下载模式。

## 这块板现在会做什么

岗哥确认过的行为：

- 语音连小智。Wi-Fi 是 `jjhome`，地址曾是 `10.197.1.66`，MQTT `api.tenclass.net:8883`。唤醒词：「七宝妹妹」「七宝」「你好七宝」。
- 摄像头不在开机或唤醒时自动开。点右上角相机按钮，或说「打开摄像头」，七宝仍是大画面，右上角是约 4:3 的实时小窗。点小窗对调大小。空白处轻点才切换对话。
- 问「是不是我」「你看到什么」「在吃什么」「在喝什么」时，工具 `self.qibao.see` 当场看一帧。本机人脸判断是不是岗哥；转正后的画面送给小智云端看图。看到可乐、汽水、奶茶，或薯片、糖果、蛋糕、巧克力、饼干，用一句温柔的话提醒少喝、少吃。
- 主人脸在 TF 卡 `/sdcard/qibao/owner.feat`，512 维，已录入。不用强制重新登记。正对镜头才会稳。仰头、侧脸时本机特征可能对不上，云端仍可能看出画面里有人。
- 音色不能在同一句话里从轻柔改成很凶。角色介绍要贴进 https://xiaozhi.me ，保存后新开一轮对话。全文在 `docs/QIBAO_VIEWER.md`。

## 源码和编译

真正编译的树是 `$HOME/esp/xiaozhi-esp32`，上游小智 v2.5.3，ESP-IDF v6.1 在 `$HOME/esp/esp-idf-v6.1`。本仓库 `firmware/qibao_s31/` 是板级副本。编译前要抄进：

`$HOME/esp/xiaozhi-esp32/main/boards/espressif/esp32-s31-korvo-1/`

云端看图用的 `EspVideo::ExplainImage` 在上游 `main/boards/common/esp_video.cc`，补丁是 `firmware/patches/0004-esp-video-explain-image.patch`。实时小窗不铺半屏静帧，相关改动在 `firmware/patches/0002-live-camera-preview.patch`。

硬件以实板为准：ESP32-S31-Korvo-1，16MB flash，16MB PSRAM，800×480 RGB 屏，GT1151 触摸，ES8389 音频。摄像头是 SC101IOT，PID `0xda4a`，输出 UYVY 640×480，不是盒子上印的 OV3660。屏当前存在 90° 竖屏。

## 接力时不要改坏的事

- 不要在板子构造函数里调用 `Board::GetInstance()`。`StartBridge` 就在构造函数里，再进去会卡死，Wi-Fi 起不来。
- 不要在预览循环里调用 `SeeFrame` 或 `EnsureCoco`，也不要开机预加载物品模型。那份 YOLO11n 约 2.9MB，内部内存还有大约 18 万时加载，也会把 SRAM 打到十几字节并重启。模型文件可以留在 TF 卡 `/sdcard/qibao/coco/`，但语音开着时不要加载。
- 人脸预加载要留着。开机内部内存大约 24 万时加载，加载后大约剩 20 万，已录入=1。不要把 `dl::Model` 的 `max_internal_size` 改成 16KB，那会分配失败然后 Load access fault。
- 不要恢复半屏静帧（`self.camera.take_photo` 那条铺屏）。实时画面只走右上角小窗。
- 问一次「看到什么」会停一下把这一帧传给云端。不要改回每几秒认一次，否则画面又会一顿一顿。

## 下一位要做的 P4

产品：咕咕两轮智行，高阶版智能中控屏，AI 虚拟交互女友。

芯片是 ESP32-P4，不是这块 S31。交互可以沿用七宝这一套：唤醒、实时小窗、认是不是主人、云端看画面、喝可乐和吃甜食时提醒、角色介绍贴在 https://xiaozhi.me 。屏、触摸、摄像头、音频、分区和引脚按 P4 那块中控屏重做。S31 这份固件只当行为样板，不能直接烧进 P4。

S31 固化版留在标签 `qibao-s31-20261120`。P4 用新分支做，不要覆盖这个标签里的 `xiaozhi.bin`。
