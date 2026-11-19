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
- **开机自动认人（照片录入）**：把岗哥本人照片编进固件（`qibao_owner_ref.h`，两张 320×240
  的不同取景帧），开机时用设备自己的模型提特征存盘，并用另一帧自检。
  实测自检相似度 **0.943**（阈值 0.55）—— 不用本人先站到镜头前录 3 张，开机就认得你。
  之后如果用摄像头录过真人，就以真人特征为准，照片不再覆盖。
- **七宝形象**：LCD 上显示**完整全身图**（站姿/坐姿两张，头到脚完整、保持比例不变形），
  屏幕背景设为与原图一致的米灰渐变使两侧无缝融合；图片以压缩 JPG 编进固件（约 75KB），
  启动时用 `jpeg_to_image_fit` 解码到 16MB PSRAM。
  **不再自动轮播**：点一下画面就在站姿/坐姿之间柔和交叉淡入淡出（0.6 秒）。
- **画面方向四挡（手动，每按一次转 90°）**：这块板没有加速度计，做不到自动跟随重力，改成手动：
  - 板载 **MODE 键长按 2 秒**
  - 屏幕**左下角的旋转按钮**（纯图形，不依赖中文字体）
  - 串口 `qrotate`（切下一挡）/ `qrot <0|90|180|270>`（直接指定）
  四个挡：**0° 横屏正向 → 90° 竖屏 → 180° 横屏倒置 → 270° 竖屏（反向）**。
  竖屏挡会把整套界面按 480×800 竖版重新排版（人物图满高、状态栏在顶、字幕条在底），
  再整体转 90° 铺满 800×480 的物理屏 —— 所以竖屏下是**真正能看内容的竖版排版**，
  不是把横屏画面转 90°（那样文字会躺倒）。
  方向会存进 NVS，重启保持。
- **整个系统跟着方向一起翻**：
  - **界面**：竖屏时按 480×800 重新布局（不是缩放）；
  - **触摸**：LVGL 命中判定自带逆变换，点哪就是哪（实测）；
  - **摄像头**：抓到的帧按当前方向去旋转，保证人脸是正的（详见下文「摄像头去旋转」）；
  - **人物图**：横屏/竖屏各解码一套尺寸，重建界面时直接复用，不重复解码。
- **串口控制台**：UART0 按行读命令，不用重烧固件就能调试：
  `help / state / qrotate / qrot <角度> / qpose / qstatus / qshot / qcam / qdiag / qprobe / wifiset <ssid> <密码> / wifilist`，
  以及 `macphone / macapps / macscreen` 等控制这台 Mac 的命令。
  `qdiag` 会打印 LVGL 变换状态并放一个洋红标记测「画面到底转没转」，是最可靠的验证手段；
  `qcam` 抓一帧摄像头画面（已去旋转），用来核对摄像头方向对不对。
- **自定义唤醒词**：`qi bao mei mei`（显示名「七宝妹妹」，MultiNet7 quant 模型）。
- **控制电脑**：复用主线的设备端 MCP 工具，走 WiFi → Mac 上 `computer_portal.py:8080`，
  应用名单动态从 Portal 端读，固件不存 App 表。11 个 `self.mac.*` 工具 + `self.qibao.face`。
- **TF 卡存储**：人脸特征优先存 `/sdcard/qibao/owner.feat`，重启不丢；卡不可用回退 NVS；
  NVS 有卡无时自动迁移到卡。启动时做读写自检。

### 屏幕方向四挡是怎么实现的（踩过坑，别改错）

这块板的屏是 **RGB 直驱 + LVGL direct 模式**：LVGL 直接把内容画进面板自己的帧缓冲，
面板再扫描输出。这条链路上 `esp_lcd_panel_mirror()` 是**不生效**的
（IDF 只在「CPU 拷贝到帧缓冲」那条路上处理镜像，而 direct 模式没有拷贝这一步）。

#### 方案选型：容器级变换（不是显示级旋转）

要支持「竖屏看内容」，有三个思路：

| 方案 | 问题 |
|---|---|
| 显示级旋转 `lv_display_set_rotation()` | esp_lvgl_port 的触摸代码**不支持**显示级旋转（`esp_lvgl_port_touch.c` 只做缩放），旋转后触摸全错位，得自己手写映射 |
| LVGL 矩阵旋转 | `LV_DRAW_TRANSFORM_USE_MATRIX` 本工程没开；开了纯软件渲染器也不消费矩阵，画面纹丝不动（实测送显缓冲校验和前后完全一样） |
| **容器级变换**（采用） | 整套 UI 塞进一个「布局根容器」，对容器做 `transform_rotation`。触摸逆变换、局部刷新都交给 LVGL 自己处理 |

**容器级变换的关键点**：竖屏时容器是 480×800，正好绕屏幕中心转 90° 就铺满 800×480 的物理屏：

```
横屏挡 (0°/180°)：容器 800×480  → 正好等于屏幕，不旋转/旋转 180°
竖屏挡 (90°/270°)：容器 480×800  → 绕屏幕中心转 90°/270° → 正好等于屏幕
```

实现（`qibao_display.h`）：

```cpp
layout_root_ = lv_obj_create(lv_screen_active());
lv_obj_set_size(layout_root_, 480, 800);
lv_obj_align(layout_root_, LV_ALIGN_CENTER, 0, 0);      // 居中 → 上下各超出 160px
lv_obj_set_style_transform_pivot_x(layout_root_, LV_PCT(50), 0);  // 变换中心 = 容器中心
lv_obj_set_style_transform_pivot_y(layout_root_, LV_PCT(50), 0);
lv_obj_set_style_transform_rotation(layout_root_, 900, 0);        // 900 = 90.0°
```

原理：LVGL 经典渲染路径里 `lv_obj_refr()` 发现对象 `rotation != 0`，
`calculate_layer_type()` 判定它是 `LV_LAYER_TYPE_TRANSFORM`，
于是先把整个对象（含所有子对象）画进一张独立图层，再由 `lv_draw_sw_transform()`
把这张图层带角度贴回主图层 —— **内容在送显缓冲里就已经是转好的，零额外 CPU 开销**。

**方向对应关系**（最容易搞混，这里写清楚）：

| 挡位 | 内容在屏上的旋转 | 设备该怎么拿 | 竖屏排版的顶边落在屏幕的 |
|---|---|---|---|
| 0° | 不转 | 横着拿 | — |
| 90° | 顺时针 90° | **设备逆时针**转 90° | 右边 |
| 180° | 倒转 | 横着拿但上下颠倒 | — |
| 270° | 逆时针 90° | **设备顺时针**转 90° | 左边 |

（90° 对应「设备逆时针转」：设备逆时针转 90° 后，物理屏的**右边**正好朝上，
而竖屏排版的顶边被映射到了物理屏的右边，所以顶边就在用户视野的最上方。）

#### 必须记住的坑

1. **`transform_pivot` 的默认值是 0（对象左上角），不是 50%。**
   不显式设成 `LV_PCT(50)` 的话，画面会绕左上角转，直接飞出屏幕，
   表现出来就是「按了旋转，画面却转不过来」。
2. **不要用 `esp_lcd_touch_set_mirror_x/y` 做触摸镜像。**
   LVGL 命中判定自带逆变换（`lv_indev_search_obj()` 先对对象做
   `lv_obj_transform_point(..., INVERSE)`，再把变换后的点传给孩子），
   驱动层再镜像一次就成了双重翻转，点了左边却触发右边。交给 LVGL 自己处理，
   保证「点哪就是哪」。
3. **不要用 `lv_display_set_rotation()` + 矩阵。**
   LVGL 9.5 的显示级矩阵旋转只在 `LV_DRAW_TRANSFORM_USE_MATRIX` 打开时生效
   （本工程 `CONFIG_LV_USE_MATRIX` 是关的），而且那条路把变换写进
   `layer->matrix -> task->matrix`，纯软件渲染器（LV_DRAW_SW）根本不消费 ——
   实测旋转前后送显缓冲校验和完全一样（`03fa93f3` -> `03fa93f3`），画面纹丝不动。
4. **【最容易踩的一个】DIRECT 模式 + 双缓冲：翻转后必须补几次整屏重画。**
   这块板 esp_lvgl_port 开了 `avoid_tearing`，LVGL 直接拿面板自己的**两块**帧缓冲
   当绘制缓冲，渲染完一块面板就切到另一块（`lv_refr.c:1404` 的 `buf_act` 交换）；
   而 LVGL 每帧**只重画失效区**。于是：

   - 翻转后第一次整屏重画只落到其中一块缓冲，另一块还留着翻转前的旧画面；
   - 面板两块轮换着扫出来 → 屏幕在「翻了 / 没翻」之间来回跳；
   - 看起来就是「按了旋转但画面转不过来」。

   修法：翻转时连着补几次整屏重画（`qibao_screen.cc` 的 `ScheduleFullRedraw()`，
   每 60ms 一次共 4 次），让两块缓冲都变成翻转后的内容。
   之后的小范围刷新只会往里面覆盖一小块**翻转后**的画面，状态就稳住了。

   ```cpp
   lv_obj_invalidate(lv_screen_active());
   s_full_redraw_left = 4;
   lv_timer_create(FullRedrawCb, 60, nullptr);  // 再补 4 次，覆盖另一块缓冲
   ```
5. **【另一个容易踩的】方向必须在 UI 建好之后才应用。**
   界面还没建就设 transform，LVGL 只会把一张空屏转一遍；
   之后各个控件创建时的小范围刷新，又会按「没翻转」的坐标把内容画回去 ——
   结果就是「开机日志明明写着 rotation=1800，画面却是正立的」（实测过）。
   所以 `SetupUI()` 里最后一步才 `ApplyRootTransform()`。
6. **【本轮新踩的，字体会变野指针导致 Guru Meditation】**
   `LcdDisplay::SetTheme()` 原来把字体设在**物理屏**上（`lv_screen_active()`）。
   竖屏适配后整套 UI 在布局根容器下面，字体得设在根容器上，否则：
   - 运行时资源包会换新字体（`LvglDisplay::SetTextFont()` → `SetTheme()`），
     换完**旧字体对象立刻被释放**；
   - 而布局根容器上还挂着旧字体的指针 → 一画字就跳到野地址 → **崩溃重启**。

   实测现象：激活页加载资源包（`Assets: Refreshing display theme...`）后 50ms 内
   `Guru Meditation: Core 1 panic'ed (Instruction access fault)`，MEPC=0x00000000，
   回溯是 `lv_font_get_glyph_dsc` → `lv_text_get_next_word` → `lv_label_event`。
   修法：`SetTheme()` 改成对 `UiParent()`（布局根容器）设字体，
   物理屏也一起设（SetupUI 之前可能已经调过一次 SetTheme，留着会挂野指针）。

   ```cpp
   lv_obj_t* screen  = lv_screen_active();
   lv_obj_t* ui_parent = UiParent();     // 竖屏时 = 布局根容器
   lv_obj_set_style_text_font(screen, text_font, 0);
   if (ui_parent != screen) {
       lv_obj_set_style_text_font(ui_parent, text_font, 0);
   }
   ```
7. **横竖屏切换 = 整套界面重建，不是转个角度。**
   横屏挡布局 800×480、竖屏挡布局 480×800，尺寸不同，控件得重新按竖版排。
   所以方向切换时：删旧 UI 对象 → 删旧布局根容器 → 按新尺寸建新容器 → 重新 `SetupUI()`。
   **人物图的解码结果按朝向分别缓存**（横屏 264×480、竖屏 448×800，各两张），
   重建时直接复用，不重新解码（竖屏那套解码要 349ms，每次切都解会明显卡顿）。
   LVGL 删除图片对象时**不会**释放 `LV_IMAGE_SRC_VARIABLE` 类型的图源，
   所以缓存的解码数据能安全跨重建存活。

所有显示相关的调用都要投递到 LVGL 线程（`lv_async_call`）。
在别的线程直接调 `lv_refr_now()` 会和 LVGL 线程互等，**直接死锁**
（实测卡在 `task_wdt`，main 任务一直不退出）——这是这一版踩的第一个坑。

#### 摄像头去旋转（人脸识别要正脸）

屏幕方向变了，说明设备被转了 90°/180°，那**摄像头拍到的画面在设备坐标系里也跟着转了** ——
人脸就是躺着的。人脸检测模型只认正脸，不转的话根本检不出来
（表现就是「转成竖屏以后刷脸一直失败」）。

做法：抓到 JPEG 解码成 RGB565 之后，按当前方向把像素缓冲旋转一次
（`qibao_persona.cc` 的 `rotate_rgb565_cw()`）：

```cpp
const int angle = qibao::CurrentAngleDeg();   // 0 / 90 / 180 / 270
// 90°/270° 时宽高互换：320x240 → 240x320
uint8_t* rot = rotate_rgb565_cw(rgb, w, h, angle, &rw, &rh);
```

转完之后画面里的「上」和竖屏界面的「上」一致，也和岗哥照片的「上」一致，特征才对得上。
（顺带说明：特征模型是用检测到的 5 个关键点做仿射对齐的，源图的长宽比差异会被对齐归一化，
所以 320×240 和 240×320 两种源图提出来的特征是可比的。）


### 怎么验证「真的转了」（这里也踩过坑，别再用错方法）

**整屏校验和不可靠**：未激活时屏幕上有 `Activating... N/10` 的重试提示，文字一直在变，
校验和既不能证明转了、也不能证明没转。

**LVGL 自带的整屏快照也不可靠**：`lv_snapshot_take(screen, ...)` 在 LVGL 9.5 里
走的是 `lv_obj_redraw()`（见 `lv_snapshot.c`）：

```c
if(top_obj == obj) { lv_obj_redraw(&layer, top_obj); }   // 整屏快照：变换被刻意跳过
else              { lv_obj_refr(&layer, top_obj); }       // 只有快照子对象才走变换
```

也就是说屏幕一旦带 transform，上游快照拍出来的还是「没翻」的画面 ——
**这正是之前误判「旋转没生效」的原因**。

**可靠判据是「洋红标记 + 全屏扫描」**：在**布局根容器的左上角**放一个洋红色方块
（放容器里而不是屏幕上，这样它会跟着界面一起转），等一帧后扫描送显缓冲，
报出洋红像素的包围盒 —— 落在哪个角，就说明当前挡位对不对。
固件里 `qdiag` 命令干的就是这件事（`qibao_screen.cc` 的 `RunMarkerTest()`）：

```
[标记] 变换前 rotation=900 pivot=(536870962,536870962) 子对象数=10 方向=90° 竖屏（设备逆时针拿）
[标记] 缓冲 800x480 stride=800
[标记] 洋红命中 3210 像素，包围盒=(742,0)-(799,58)，中心=(770,29)
[标记] 标记在右上 => 90°（顺时针），当前方向 90° 竖屏（设备逆时针拿）
```

> 标记要放在**布局根容器**里，而且创建之后要**整屏重画**一遍：
> 竖屏时容器有一半在屏幕外，只做局部队列刷新可能刷不到变换图层
> （实测 90° 挡一开始标记就没画出来，加了整屏重画 + 双缓冲补画后才稳定）。

七宝自己的快照（`QibaoLcdDisplay::SnapshotToJpeg`）也改成**直接读送显缓冲**，
绕开 LVGL 那条会跳过变换的快照路径，拍到的就是屏幕上真实显示的内容。

> 顺带修掉的一个颜色 bug：上游 `LvglDisplay::SnapshotToJpeg()` 在交给编码器前
> 多做了一次 `__builtin_bswap16()`，那是给 `RGB565_SWAPPED` 的屏用的；
> 本板是普通 RGB565，多换一次会让背景渐变变成彩虹色带。编码器内部对 RGB565
> 是直接 memcpy（`ESP_IMGFX_PIXEL_FMT_RGB565_LE`），**不要再换**。


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

形象图资源头文件 `qibao_frames.h` 由 `tools/jpg2c.py` 把原始 JPG 字节转成 C 数组
（不做解码，固件运行时解码到 PSRAM）。换形象图：先用 `sips` 把完整全身图等比缩到
540×960、JPG 质量 55（每张约 30~45KB），再逐个转成数组：

```bash
sips -z 960 540 qibao_standing.jpg --out /tmp/qb_frame0.jpg -s format jpeg -s formatOptions 55
sips -z 960 540 qibao_seated.jpg   --out /tmp/qb_frame1.jpg -s format jpeg -s formatOptions 55
python3 tools/jpg2c.py /tmp/qb_frame0.jpg \
  <工程>/main/boards/espressif/esp32-s31-korvo-1/qibao_frames.h qibao_frame_0_jpg
# 第二张追加进同一头文件（数组名 qibao_frame_1_jpg）
```

`qibao_display.h` 用 `jpg/jpeg_to_image.h` 的 `jpeg_to_image_fit(jpg, len, 800, 480, ...)`
解码，输出约 264×480（按 1080:1920 比例、高度铺满），居中显示。
旧的 `qibao_avatar.h`（360×360 RGB565）已废弃，不再被引用。

## 烧录

用 `prebuilt/qibao_s31/flash.sh`（或 `tools/flash_s31_qibao.sh`）：

```bash
bash prebuilt/qibao_s31/flash.sh /dev/cu.usbserial-2110 460800
```

分区布局（`16m_qibao.csv`）：app 分区 `ota_0` 7.875MB（含人脸模型），assets 8MB。
**刻意不写 NVS 分区**，保留配网信息和人脸特征。

## 实测验证状态（2026-11-19，四挡方向 + 竖屏排版 + 照片认人一版）

固件重新编译、烧录、抓完整串口启动日志验证通过（`0x77af00` = 7.8MB，app 分区剩 5%）：

- **无崩溃无重启**：`Abort/Guru/Backtrace/Rebooting` 全部没有。
  （中途修掉一个真崩：**资源包换字体后旧字体被释放，而布局根容器还挂着旧字体指针** →
  激活页一画字就 `Guru Meditation` 无限重启。见上面第 6 条坑。）
- **四挡方向全部用洋红标记验证到位**（标记放在布局根容器左上角，扫描送显缓冲找洋红像素）：

  | 挡位 | 布局 | 洋红包围盒 | 判定 |
  |---|---|---|---|
  | 0° | 800×480 | `(0,0)-(59,59)` 左上 | 正向 ✓ |
  | 90° | 480×800 | `(742,0)-(799,58)` 右上 | 顺时针 ✓ |
  | 180° | 800×480 | `(742,422)-(799,479)` 右下 | 倒置 ✓ |
  | 270° | 480×800 | `(0,422)-(58,479)` 左下 | 逆时针 ✓ |

- **快照肉眼核对**（`qshot` 抓回电脑）：
  - 0°/180°：全身像居中占满高度，旋转按钮在画面左下（180° 时在右上）；
  - 90°/270°：人物图满高铺满竖版画布，整体侧躺 90° —— 设备逆时针/顺时针拿起来正好正立；
  - 背景米灰渐变正常（没有彩虹色带）。
- **触摸命中跟着方向走**：LVGL 命中判定自带逆变换（`lv_indev_search_obj`），
  容器级变换下实测「点哪就是哪」；无头 LVGL 复现验证：容器带 90° 变换时，
  点屏幕上按钮的可见位置能正确触发容器里那个按钮。
- **局部刷新位置正确**：竖屏时画布有一半在屏幕外，
  无头 LVGL 复现验证「画布顶部（屏幕上边之外）」和「画布底部（下边之外）」的控件
  局部刷新，都会正确落到屏幕的右/左边缘 —— 也就是竖屏下状态栏和聊天文字能正常更新。
- **照片认人自检通过**：
  `【七宝自检】用岗哥另一张不同取景的照片比对，相似度=0.943（阈值 0.55）=> 认出是岗哥 ✓`
  → `岗哥人脸已由照片录入（成功），门禁已生效`；特征同时写进 NVS 和 TF 卡。
- **方向可持久化**：NVS 存角度，开机自动恢复（实测存 90° 时开机日志直接是
  `恢复上次保存的方向：90° 竖屏（设备逆时针拿）`）。
- **串口控制台可用**：`qstatus` 返回
  `{"enrolled":true,"unlocked":false,"remaining_ms":0,"similarity":0.943,"sdcard":true}`；
  `qpose` 站姿↔坐姿切换正常；`qrotate` / `qrot <角度>` 方向切换正常。
- **WiFi 已连上 jjhome**，拿到 IP `10.197.1.66`（凭据已在 NVS，重启自动连）。
- **TF 卡正常**：`TF 卡挂载成功：USD00（400 kHz, 4-bit）` + `读写自检：通过`。

### 已知未解决：摄像头驱动没开（既有问题，与方向无关）

`qcam` 抓帧报 `EspVideo: open /dev/video2 failed, errno=2(No such file or directory)` ——
**DVP 视频设备节点根本没生成**，也就是说这块板上的摄像头从来没通过。

根因：板级 `config.json` 里写了 `"sdkconfig_append": ["CONFIG_CAMERA_OV3660=y"]`，
但本工程是用 `sdkconfig.defaults;sdkconfig.defaults.esp32s31` 两个 defaults 文件生成 sdkconfig 的，
`config.json` 的 `sdkconfig_append` **不在这条路径上生效**，
最终 sdkconfig 里是 `# CONFIG_CAMERA_OV3660 is not set` → 传感器驱动没编进固件 → DVP 驱动绑不上 → 没有 `/dev/video2`。

修的话要在 `sdkconfig.defaults.esp32s31` 里显式加：
`CONFIG_CAMERA_OV3660=y` + 选一个 JPEG 输出格式（如 `CONFIG_CAMERA_OV3660_DVP_JPEG_320X240_25FPS=y`）
+ `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y`（`EspVideo::Capture()` 只在开了这个选项时才走 JPEG 分支，
而人脸识别那侧要的就是 JPEG 数据）。

在修好之前：**开机用照片认人是好的（走的是固件内置照片，不碰摄像头），
但「站到镜头前刷脸开门」是不通的**。

## 待办 / 需用户本人操作

1. **激活设备（下一步只差这一步）**：这台板子还没在小智云端激活。
   串口会打印 `Activation: xiaozhi.me` + 一个 6 位数字码（实测稳定是 **`073092`**）。
   需要本人用小智账号打开 xiaozhi.me（或小智 App）→ 添加设备 → 输入这个码完成激活，
   激活后语音链路才能通。这是账号操作，Agent 代不了。
   **为什么手机搜不到设备热点**：板子已经连上家里的 jjhome（IP `10.197.1.66`），
   连上网之后它就不再开热点，所以手机 WiFi 列表里看不到它 —— 这是正常的，
   绑定走的是「激活码」而不是「连热点」。
2. **唤醒词实测**：固件里 `qi bao mei mei` 已编进去，但要在连网进入 listening 后才会真正加载，
   需要对着板子喊一次确认。
3. **肉眼确认四挡方向**：数值和快照都已证明方向生效（见上），但实体屏幕还得本人看一眼：
   长按 MODE 键 2 秒（或点屏幕上的圆形箭头按钮），每按一次转 90°，
   依次是「横屏正向 → 竖屏（人物侧躺 90°，设备逆时针拿正立）→ 横屏倒置 → 竖屏（反方向）」。
   竖屏挡时人物图会满高铺满画面、状态栏在顶、字幕条在底，按着不放别松手，
   顺手确认一下触摸位置也跟着对（点图片换姿势、点左下角按钮转方向）。
4. **摄像头刷脸还打不通**：驱动没编进固件（见上面「已知未解决」），
   需要把 `CONFIG_CAMERA_OV3660=y` 等几项加进 `sdkconfig.defaults.esp32s31` 重新编译。
   在那之前只有「开机用照片认人」是通的。
5. **形象图更多互动表情**（说话高亮、开心/生气切换）可在 `qibao_display.h` 的 `SetupUI()` 基础上扩展。


## 关键踩坑（改动时留意）

- **板型必须锁死** `CONFIG_BOARD_TYPE_ESP32_S31_KORVO_1=y`，否则 Kconfig 默认选 FUNCTION_COREBOARD_1
  （ES8311），启动写 I2C 失败 abort 无限重启。
- 固件含人脸模型达 7.5MB，标准 v2/16m.csv 只给 app 4MB 塞不下 → 必须用 `16m_qibao.csv`。
- ESP-IDF 禁 RTTI，`EspVideo*` 转换用 `static_cast` 不用 `dynamic_cast`。
- TF 卡不要操作 GPIO39（那不是电源控制脚，拉高会导致 SDMMC 超时）；用探测频率 400kHz 初始化。
- 抓串口启动日志：macOS 无 `timeout`；RTS/DTR 组合会把芯片拉进下载模式。
  正确做法是先开 pyserial 串口，再子进程跑 `esptool --after hard_reset chip-id`，从 ROM 开始抓。
