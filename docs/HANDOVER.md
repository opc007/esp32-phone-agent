# 七宝妹妹（ESP32-S31-Korvo-1）交接文档

> 面向接手的人（尤其是用 Cursor 继续做的人）。读完这一篇应该能直接上手，不用翻聊天记录。
> 最后更新：2026-11-19，代码已同步到 GitHub `opc007/esp32-phone-agent` main，commit `8c83f9c`。

---

## 0. 一分钟速览

| 项 | 值 |
|---|---|
| 目标板 | ESP32-S31-Korvo-1（800×480 RGB LCD + GT1151 触摸 + ES8389 codec + OV3660 DVP 摄像头） |
| 上游工程 | [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.5.3，基线 commit `a73e9a7` |
| 本地开发树 | `~/esp/xiaozhi-esp32`（= 上游 clone + 我们的改动） |
| IDF | ESP-IDF v6.1（`~/esp/esp-idf-v6.1`），C++ 要求 `-std=gnu++23` |
| 本仓库 | `/Users/mac007/Documents/esp32控制手机`，远端 `opc007/esp32-phone-agent` |
| 串口 | `/dev/cu.usbserial-2110`（CP2102N），波特 115200（控制台）/ 460800（烧录） |
| 设备 IP | `10.197.1.66`（WiFi `jjhome`，凭据已在 NVS） |
| 唤醒词 | `qi bao mei mei`（MultiNet7） |
| 当前固件 | `0x77ca80`（app 分区剩 5%），已烧录在设备上 |
| 激活码 | `073092`（还没在小智云端激活，需用户本人操作） |

**当前状态一句话**：屏幕四挡旋转（0/90/180/270）+ 竖屏重排 + 摄像头（SC101IOT，UYVY→RGB565，按方向去旋转）
**已全部做完并真机验证**。剩下的都是「需要人」的事：小智云端激活、肉眼确认屏幕、站在镜头前试一次刷脸。

---

## 1. 目录关系（先搞清楚哪份代码是哪份）

```
~/esp/xiaozhi-esp32/                     ← 真正用来编译的开发树（上游 clone + 改动）
├── main/boards/espressif/esp32-s31-korvo-1/    ← 板级代码（七宝全部逻辑在这）
│   ├── qibao_screen.h/.cc      方向状态机 / NVS 持久化 / 标记诊断
│   ├── qibao_display.h         显示层（布局根容器 / 人物图缓存 / 重建）
│   ├── qibao_persona.h/.cc     人设 + 人脸门禁 + 摄像头帧去旋转
│   ├── qibao_console.h/.cc     串口控制台（qrotate/qrot/qcam/...）
│   ├── qibao_sdcard.h/.cc      TF 卡（人脸特征存卡）
│   ├── qibao_frames.h          内置图片数据
│   ├── qibao_owner_ref.h       本人照片参考数据
│   ├── esp32_s31_korvo_1.cc    板级主类（显示/触摸/摄像头初始化）
│   └── config.h                引脚表（**没改过，跟上游一致**）
├── main/display/lcd_display.h/.cc          ← 改了（运行时布局尺寸 / 根容器 / 重建）
├── main/CMakeLists.txt / application.cc / protocols/protocol.cc / idf_component.yml  ← 改了
├── main/boards/common/esp_video.h          ← 改了（允许 JPEG 输入）
└── sdkconfig.defaults.esp32s31             ← 改了（板型锁 / 唤醒词 / 分区表 / 摄像头）

/Users/mac007/Documents/esp32控制手机/     ← 交付仓库（GitHub 那个）
├── firmware/qibao_s31/          板级文件副本（上游没有的那些 + 改过的那些）
├── firmware/patches/0001-integrate-phone-agent.patch   ← 上游非板级文件的 diff
├── firmware/board/              S3 那条线（手机直连）的板级文件，与七宝无关
├── prebuilt/qibao_s31/          预编译产物（xiaozhi.bin 等）+ flash.sh
├── tools/                       构建/烧录/调试脚本
└── docs/QIBAO_S31.md            七宝的完整技术文档（细节比这篇细，改代码前建议读）
```

### 仓库里两份「同源文件」的关系（最容易踩坑）

- `firmware/qibao_s31/` = 板级目录的副本。上游 `main/boards/espressif/esp32-s31-korvo-1/` 里
  **已存在的文件**（`config.h`、`config.json`、`bsp_adc_calibration.h`、`esp32_s31_adc_calibration.c`、`README.md`）
  不进仓库（走 patch 或本来就一致）；**我们新增/修改的**进仓库。
- `firmware/patches/0001-integrate-phone-agent.patch` = 上游**非板级**文件的 diff，
  已用 `git worktree` 在基线 `a73e9a7` 上验证可干净应用。
  **改上游非板级文件后必须重新生成这个补丁**，否则别人从仓库复现不出来。

重新生成补丁的命令（务必精确列文件，别把 `bread-compact-wifi-s3cam` 那条 S3 线带进去）：

```bash
cd ~/esp/xiaozhi-esp32
git diff HEAD -- \
  main/CMakeLists.txt main/application.cc main/boards/common/esp_video.h \
  main/display/lcd_display.cc main/display/lcd_display.h \
  main/idf_component.yml main/protocols/protocol.cc \
  sdkconfig.defaults.esp32s31 \
  > /Users/mac007/Documents/esp32控制手机/firmware/patches/0001-integrate-phone-agent.patch
```

验证补丁能干净应用：

```bash
cd ~/esp/xiaozhi-esp32
git worktree add --detach /tmp/xz_test HEAD
cd /tmp/xz_test && git apply --check <patch> && echo OK
cd ~/esp/xiaozhi-esp32 && git worktree remove /tmp/xz_test --force
```

---

## 2. 构建 / 烧录 / 调试（三条命令记住就够）

### 构建

```bash
bash /Users/mac007/Documents/esp32控制手机/tools/build_s31_qibao.sh
```

- 独立 build 目录 `~/esp/xiaozhi-esp32/build_s31`，独立 sdkconfig `build_s31/sdkconfig`，
  defaults 是 `sdkconfig.defaults;sdkconfig.defaults.esp32s31` 两个。
- 板型靠 `-DBOARD_NAME=esp32-s31-korvo-1` + `CONFIG_BOARD_TYPE_ESP32_S31_KORVO_1=y` 双重锁定。

### 烧录（**绝不写 NVS**，否则配网和人脸特征全丢）

```bash
cd ~/esp/xiaozhi-esp32/build_s31
~/.espressif/python_env/idf6.1_py3.13_env/bin/esptool --chip esp32s31 \
  --port /dev/cu.usbserial-2110 --baud 460800 \
  --before default_reset --after hard_reset \
  write_flash -z --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x20000 xiaozhi.bin
```

或者用仓库里的脚本（会连 bootloader/分区表/资源一起刷，同样不碰 NVS）：

```bash
bash /Users/mac007/Documents/esp32控制手机/prebuilt/qibao_s31/flash.sh /dev/cu.usbserial-2110 460800
```

分区表：`partitions/v2/16m_qibao.csv`（app 7.875MB + assets 8MB）。标准 `v2/16m.csv` 只有 4MB app，塞不下人脸模型。

### 调试工具（都在 `tools/`）

| 工具 | 用途 |
|---|---|
| `serial_capture.py --reset --seconds 45 --out /tmp/boot.log` | 复位抓启动日志。**会复位设备** |
| `qshot_dir.py --dir 0\|90\|180\|270 --out /tmp/q.jpg --diag` | 切方向 + 抓屏幕快照 + 洋红标记定位诊断。**不复位** |
| `qcam_dir.py --dir 0\|90\|180\|270 --out /tmp/cam.jpg` | 切方向 + 抓摄像头帧（当前会失败，见第 4 节） |
| `qshot.py --out /tmp/a.jpg` | 普通快照 |
| `qcmd.py <命令>` | 通用：发任意串口控制台命令并打印回复（**新增**） |
| `make_owner_ref.py` | 用本人照片生成 `qibao_owner_ref.h` 参考数据 |
| `verify_rotation.py` / `shot_compare.py` / `jpg2c.py` | 旋转验证 / 快照对比 / 图片转 C 数组 |

**串口三条铁律**：
1. 打开串口会拉动 DTR/RTS → **设备被复位一次**。所有脚本都是「打开后先把 dtr/rts 置低，再轮询 `qstatus` 等就绪」。
2. **串口独占**，同一时刻只能跑一个抓取脚本，并发会 `device disconnected`。
3. macOS **没有 `timeout` 命令**（GNU coreutils 才有）。

---

## 3. 已经做完并验证过的东西

### 3.1 屏幕四挡方向（用户核心诉求）

需求原文：「屏幕按旋转也不能把显示的画面转过来啊。可以每按一次选择一次，直到用户满意。」
「不对啊。旋转图片，可以90度转的每一次啊。而不是让你反转180度。我需要竖屏看内容的。
系统跟着翻，不但是图片，包括摄像头和界面交互，都要跟着适配反正横屏还是竖屏。」

实现：**容器级 LVGL 变换**。整套 UI 塞进一个「布局根容器」，对容器做
`lv_obj_set_style_transform_rotation()`。横屏挡容器 800×480，竖屏挡容器 480×800，
绕屏幕中心转 90°/270° 后正好铺满物理屏。

**为什么不用 `lv_display_set_rotation()`**：esp_lvgl_port 的触摸代码不支持它，触摸会全错位。
**为什么不用 LVGL 矩阵路径**（`LV_DRAW_TRANSFORM_USE_MATRIX`）：矩阵写进 `layer->matrix`，
纯软件渲染器根本不消费，画面纹丝不动（实测送显缓冲校验和前后完全一样）。

关键代码：
- `main/display/lcd_display.h:45-82` —— 新增 `LayoutW()` / `LayoutH()` / `UiParent()` /
  `SetLayoutRoot()` / `ClearUIObjects()` / `RebuildUI()`。
- `main/display/lcd_display.cc` —— `SetupUI()` 全部尺寸改用 `LayoutW()/LayoutH()`，
  `auto screen = UiParent()`。
- `qibao_display.h:230/259/274` —— `EnsureLayoutRoot()` / `DestroyLayoutRoot()` / `ApplyRootTransform()`。
- `qibao_display.h:99` —— `ApplyRotation()`：尺寸变了 → `ClearUIObjects()` → `DestroyLayoutRoot()`
  → `EnsureLayoutRoot()` → `setup_ui_called_=false` → `SetupUI()`。**换挡是整套重建，不是转个角度**。

方向语义（**顺时针为正**）：

| 挡位 | 布局尺寸 | 画面 | 拿到手 |
|---|---|---|---|
| 0° | 800×480 | 正向 | 横屏，正常拿 |
| 90° | 480×800 | 内容顺时针转 90° | **设备逆时针拿**才正立 |
| 180° | 800×480 | 倒置 | 横屏倒着拿 |
| 270° | 480×800 | 内容逆时针转 90° | 设备顺时针拿 |

真机验证（洋红标记放在布局根容器左上角 + 全屏扫描报包围盒）：

| 挡位 | 洋红包围盒 | 判定 |
|---|---|---|
| 0° | (0,0)-(59,59) 左上 | 正向 ✓ |
| 90° | (742,0)-(799,58) 右上 | 顺时针 ✓ |
| 180° | (742,422)-(799,479) 右下 | 倒置 ✓ |
| 270° | (0,422)-(58,479) 左下 | 逆时针 ✓ |

新固件上已回归复测 0° 与 90°，结果同上。方向存 NVS（新键 `rot`，迁移旧的 `rot180`），
开机自动恢复（实测日志：`恢复上次保存的方向：90° 竖屏（设备逆时针拿）`）。

**踩坑**：标记必须放在**布局根容器**内，且创建后要**整屏重画** ——
只做局部队列刷新可能刷不到变换图层（90° 挡曾因此测不出标记）。
翻转后保留「连补 4 次整屏重画」的双缓冲补丁（`ScheduleFullRedraw()`）。

**踩坑（会导致崩溃）**：`LcdDisplay::SetTheme()` 原先把字体设在物理屏 `lv_screen_active()` 上。
竖屏适配后 UI 在布局根容器下，资源包换字体时旧字体对象被释放 → 根容器留野指针 →
一画字就 `Guru Meditation: Core 1 panic'ed (Instruction access fault)`（MEPC=0，
回溯 `lv_font_get_glyph_dsc` → `lv_text_get_next_word` → `lv_label_event`），设备陷入崩溃循环。
**修法**：改设在 `UiParent()`，物理屏也一并设。

### 3.2 摄像头帧跟随方向去旋转

`qibao_persona.cc:229` —— `rotate_rgb565_cw()`：摄像头帧按 `qibao::CurrentAngleDeg()` 去旋转，
90°/270° 时 320×240 → 240×320。保证人脸识别拿到的是正脸。
参考照片方向视为 0° 基准，特征模型用 5 点关键点仿射对齐，长宽比差异会被归一化。

### 3.3 其它已完成

- 站姿/坐姿手动切换（`qpose`），不自动轮播。
- 本人照片开机即识人（走固件内置照片 `qibao_owner_ref.h`，照片自检相似度 0.943，阈值 0.55）。
- TF 卡挂载（人脸特征存卡）：`TF 卡挂载成功：USD00（400 kHz, 4-bit）` + `读写自检：通过`。
- 设备端 MCP 工具（WiFi 控制这台 Mac）。
- 串口控制台：`help / state / qrotate / qrot <0|90|180|270> / qpose / qstatus / qprobe / qdiag /
  qshot / qcam / qface / wifiset <ssid> <密码> / wifilist / macphone / macapps / macscreen`。
  `qface` 是新增的：不依赖云端直接刷脸（设备还没激活时云端工具走不通）。

---

## 4. 摄像头：已打通（板上装的是 SC101IOT，不是 OV3660）

**结论：这块板子上的摄像头模组是 SC101IOT（SCCB 地址 0x68、PID 0xda4a），不是盒子上印的 OV3660（0x3c）。**

乐鑫自己在上游记了同一个现象：

- `espressif/esp-bsp#840`「ESP32-S31-Korvo-1: OV3660 camera XCLK (GPIO55) not toggling, sensor detect fails on chip rev v0.0」
  —— 环境与我们完全一致（S31 rev v0.0 + IDF v6.1 + 官方未改示例）。
- 乐鑫协作者 espzav 回复：*You may have another camera sensor. It seems that some boxes has another camera sensor and not mentioned on the box print.*
  修复 PR：`espressif/esp-bsp#831`（给 BSP 补上 SC101IOT 支持）。

### 当时是怎么修的（三层）

1. **驱动没编进固件**：板级 `config.json` 的 `"sdkconfig_append": ["CONFIG_CAMERA_OV3660=y"]`
   在本工程的 sdkconfig 生成路径（两个 defaults 文件）上不生效，
   必须显式写进 `sdkconfig.defaults.esp32s31`：
   `CONFIG_CAMERA_OV3660=y` / `CONFIG_CAMERA_OV3660_AUTO_DETECT_DVP_INTERFACE_SENSOR=y` /
   `CONFIG_CAMERA_OV3660_DVP_JPEG_320X240_25FPS=y` /
   `CONFIG_CAMERA_OV3660_DVP_DEFAULT_FMT_JPEG_320X240_25FPS=y` /
   `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y`。

2. **OV3660 那颗芯片根本不在板上**（真正的根因）。
   把 SC101IOT 也编进去，两个驱动并存，esp_video 会在 DVP 端口逐个尝试
   （`esp_video_init.c` 里 `esp_cam_sensor_detect_get_array()` 拿到全部 detect 函数后遍历），
   不用事先知道装的是哪颗：
   `CONFIG_CAMERA_SC101IOT=y` / `CONFIG_CAMERA_SC101IOT_AUTO_DETECT_DVP_INTERFACE_SENSOR=y` /
   `CONFIG_CAMERA_SC101IOT_DVP_YUV422_640X480_15FPS=y` /
   `CONFIG_CAMERA_SC101IOT_DVP_YUV422_1280X720_15FPS=n` /
   `CONFIG_CAMERA_SC101IOT_DVP_DEFAULT_FMT_YUV422_640X480_15FPS=y` /
   `CONFIG_CAMERA_SC101IOT_POWER_ON_DELAY_US=20000`。

3. **SC101IOT 输出 YUV422(UYVY)，没有 JPEG** —— 原来「Capture() 拿 JPEG → 解码」的路子废了。
   在 `qibao_persona.cc` 加了 UYVY → RGB565：
   - `yuv_to_rgb565()`：BT.601 全范围整数近似，输出 `val=(R<<11)|(G<<5)|B` **小端**存放。
     字节序必须和 `jpeg_to_image_fit()` 出来的 RGB565LE 一致（它也是 `out_buf[2i]=低字节`），
     否则编码侧 `image_to_jpeg_cb(V4L2_PIX_FMT_RGB565)`（走 `ESP_IMGFX_PIXEL_FMT_RGB565_LE`）
     会红蓝颠倒。
   - `uyvy_to_rgb565()`：UYVY 每 4 字节 2 像素（`U0 Y0 V0 Y1`，同对像素共用色度），
     最近邻缩到 320×240。
   - `capture_rgb565()` 改成按实际帧格式分流：JPEG 走老路、UYVY 走新路、其它格式报错。

另外照官方 BSP 补了 20ms 上电稳定延时
（`esp-bsp` 的 `bsp_camera_start()` 就是「起 XCLK → `vTaskDelay(20ms)` → 探测」）。

### 实测

```
I Qibao: Captured frame: 614400 bytes, 640x480, format=0x59565955   ('UYVY')
I Qibao: UYVY -> RGB565 320x240
I Qibao: Frame de-rotated 90° -> 240x320
```

- 0°：320×240 正常房间画面，颜色正确（没有红蓝颠倒）。
- 90°：240×320 人脸特写，**脸是正立的**（跟着屏幕方向一起转）。
- 新增串口命令 `qface`：不依赖云端直接刷脸，返回识别结果 + 相似度 + 是否解锁。
  设备还没在小智云端激活，云端那条 `self.qibao.face` 工具走不通，所以这个命令很有用。

### 还没做但已知的两件事

1. **实时刷脸要人站在镜头前才验得了**：
   代码链路已经全通（`qface` 能抓到帧、跑检测、如实报告「No face detected」或给出相似度），
   但开发时镜头前没人脸，所以只验证到「画面正确 + 检测器正常工作」。
   参考照片比对是 0.943（阈值 0.55）通过。
2. **`esp_video` 版本对齐**：本地 `main/idf_component.yml` 写的是 `^2.4.1`，实际解析到 2.5.0，
   官方 BSP 钉的是 `~2.4.1`。**当前能正常工作，不用动**；真要对齐就改 `~2.4.1` 后删
   `dependencies.lock` 重解析。

### 上游参考（要查就查这里）

| 资料 | 地址 |
|---|---|
| 现象相同的 issue | https://github.com/espressif/esp-bsp/issues/840 |
| 官方修复 PR | https://github.com/espressif/esp-bsp/pull/831 |
| 官方 BSP 摄像头实现 | https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_s31_korvo_1/src/bsp_camera.c |
| 出厂 factory_demo（两个驱动都开） | https://github.com/espressif/esp-dev-kits/tree/master/examples/esp32-s31-korvo/examples/factory_demo |


## 5. 需要用户本人操作的事（Agent 代不了）

1. **激活设备（最优先，只差这一步）**
   串口会打印 `Activation: xiaozhi.me` + 6 位码（实测稳定是 **`073092`**）。
   用小智账号打开 xiaozhi.me 或小智 App → 添加设备 → 输入这个码。
   激活后语音链路才通。
   *手机搜不到设备热点是正常的*：板子已连上家里的 `jjhome`（IP `10.197.1.66`），
   连网后就不开热点了，绑定走激活码而不是连热点。

2. **肉眼确认四挡方向**（数值和快照都证明了，但实体屏还得人眼看一下）
   长按 MODE 键 2 秒，或点屏幕上的圆形箭头按钮，每按一次转 90°：
   横屏正向 → 竖屏（设备逆时针拿）→ 横屏倒置 → 竖屏（反方向）。
   顺便确认触摸位置也跟着对（点图片换姿势、点左下角按钮转方向）。

3. **摄像头刷脸实时识别**：链路已通（见第 4 节），**差一步：人要站在镜头前**。
   开发时镜头前没人脸，只验证到「画面正确 + 检测器正常工作 + 参考照片比对 0.943 通过」。
   插上串口敲 `qface` 就能看识别结果和相似度。

---

## 6. 踩坑清单（改代码前务必看一眼）

- **板型必须锁死** `CONFIG_BOARD_TYPE_ESP32_S31_KORVO_1=y`，
  否则 Kconfig 默认选 FUNCTION_COREBOARD_1（ES8311），启动写 I2C 失败 abort 无限重启。
- **固件含人脸模型达 7.5MB**，标准 `v2/16m.csv` 只给 app 4MB 塞不下 → 必须用 `16m_qibao.csv`。
- **ESP-IDF 禁 RTTI**，`EspVideo*` 转换用 `static_cast` 不用 `dynamic_cast`。
- **TF 卡不要操作 GPIO39**（那不是电源控制脚，拉高会导致 SDMMC 超时）；用探测频率 400kHz 初始化。
- **LVGL 矩阵旋转必须关**（`CONFIG_LV_USE_MATRIX` / `CONFIG_LV_DRAW_TRANSFORM_USE_MATRIX`），
  开着无效还占资源。
- **`ESP_LOGx(tag, format, ...)` 一定要带 `tag`！**
  漏了会报一堆看不懂的错：`expected ')' before 'GPIO_NUM_55'` /
  `expected ')' before 'esp_err_to_name'`。因为第一个参数会被当成 tag，
  真正的 format 变成了 `GPIO_NUM_55`，再套进 `LOG_FORMAT()` 宏就成了非法表达式。
  （这个坑我踩过一次，白排查了很久。）
- **烧录绝不能写 NVS**：否则 WiFi 配网和小智激活码、人脸特征全丢。
- **仓库根目录有 38 个无关未跟踪文件**，`tools/` 下也有一批未跟踪脚本 —— **`git add` 必须精确列文件**，
  不要 `git add .` / `git add -A`。
- **macOS 没有 `timeout` 命令**；RTS/DTR 组合会把芯片拉进下载模式。
  抓启动日志的正确做法：先开 pyserial 串口，再子进程跑 `esptool --after hard_reset chip-id`，
  从 ROM 开始抓。

---

## 7. 仓库卫生（提交时务必照做）

```bash
cd /Users/mac007/Documents/esp32控制手机
git status --short          # 先看清楚哪些是本次改动
git add <精确文件列表>       # 绝不 git add . / -A
git commit -m "..."
git push origin main
```

本仓库同时放着两条线的东西，**别搞混**：
- **七宝 S31 线**（本文件的主角）：`firmware/qibao_s31/`、`prebuilt/qibao_s31/`、`docs/QIBAO_S31.md`、
  `tools/build_s31_qibao.sh`、`tools/flash_s31_qibao.sh`、`tools/qshot*.py`、`tools/qcam_dir.py`、`tools/qcmd.py`。
- **S3 手机直连线**（历史遗留，仍在用）：`firmware/board/`、`firmware/sdkconfig.board`、
  `tools/*phone*`、`tools/*hid*`、`tools/*portal*` 等。

改上游非板级文件 → **一定要重新生成 `firmware/patches/0001-integrate-phone-agent.patch`**（命令见第 1 节）。
改板级文件 → **一定要同步到 `firmware/qibao_s31/`**。

---

## 8. 给 Cursor 的上手建议

**现在没有硬技术缺口了**，剩下的活基本是：调人设话术、加表情、切 90° 后的界面细节、
把云端激活流程跑通。如果要继续加深，按下面读。

如果要动摄像头相关代码，按这个顺序读：

1. `~/esp/xiaozhi-esp32/main/boards/espressif/esp32-s31-korvo-1/esp32_s31_korvo_1.cc`
   → `InitializeCamera()`，看 SCCB/XCLK 现在怎么配的。
2. `~/esp/xiaozhi-esp32/managed_components/espressif__esp_cam_sensor/sensors/ov3660/ov3660.c`
   → `ov3660_detect()` / `ov3660_power_on()` / `ov3660_get_sensor_id()`，
   看它到底怎么读 PID、在哪一步失败。
3. `~/esp/xiaozhi-esp32/managed_components/espressif__esp_video/src/esp_video_init.c`
   → `esp_video_init()` 里的传感器探测流程、`init_dvp_clk_func` 接线位置（约 962 行）。
4. `~/esp/xiaozhi-esp32/managed_components/espressif__esp_video/include/esp_video_init.h`
   → `esp_video_init_sccb_config_t`（`init_sccb` / `i2c_handle` / `freq` 三个字段的含义）。
5. `~/esp/esp-idf-v6.1/components/esp_driver_cam/dvp/include/esp_private/esp_cam_dvp.h`
   → `esp_cam_ctlr_dvp_start_clock()` 等 DVP 私有 API。

调试时的观察点：
- 启动日志 `sc101iot: Detected Camera sensor PID=0xda4a` → 传感器认出来了，摄像头通。
- `Failed to detect camera sensor with address=3c` → OV3660 不应答（正常，这颗芯片不在板上）；
  只有连 SC101IOT 的 `address=68` 也失败，才是真有问题。
- `Captured frame: ... format=0x59565955` → `0x59='Y' 0x56='V' 0x59='Y' 0x55='U'` 即 UYVY 格式。
- `No face detected` → 画面里确实没人脸（检测器如实报告，不会瞎报相似度）。

### 抓帧脚本的老坑（已修，记一下）

- `qcam_dir.py` 原来用 `\[CAM\]` 去匹配 base64，但设备端的 `PrintBase64()` 是屏幕快照和
  摄像头抓帧**共用**的，固定打 `[SHOT]` 前缀 —— 所以永远匹配不到。已改成匹配 `[SHOT]`。
- 设备刚开机那几秒日志刷得极多，串口控制台任务如果正卡在耗时操作（比如切方向时整套 UI 重建），
  UART 接收缓冲会被日志冲掉，命令直接丢。`qcam_dir.py` 现在会盯着设备回显 `[CMD] > qcam`，
  几秒没看到就重发（最多 3 次）。

改完固件后回归四挡方向（别只顾摄像头把方向改坏了）：

```bash
python3 tools/qshot_dir.py --dir 0  --out /tmp/v0.jpg  --diag   # 期望洋红在 (0,0)-(59,59)
python3 tools/qshot_dir.py --dir 90 --out /tmp/v90.jpg --diag   # 期望洋红在 (742,0)-(799,58)
```
