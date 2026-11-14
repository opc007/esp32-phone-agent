# ESP32 直连手机：把所有控制逻辑放进固件

> 目标：**几十块钱的 ESP32 + 一根 C 口线 + 语音，直接控制手机**。链路里没有电脑。
> 本文是当前最终方案，取代 `PHONE_CONTROL.md`（手机接 Mac）和 `PHONE_DIRECT.md`（Termux 自托管）。

---

## 1. 先回答那个问题

**「为什么不把全部需求写进 ESP32，接上手机自动装好插件？」**

拆成两半看：

| 诉求 | 能不能 | 原因 |
|---|---|---|
| 控制逻辑、中文整句编排、读屏、点击、二次确认全放进 ESP32 | ✅ 已做到 | 固件里新增 `phone_bridge.cc`，云端小智只管对话 |
| 引导页、APK 下载、手机 IP 自动发现、自动绑定全由 ESP32 做 | ✅ 已做到 | ESP32 自己跑 HTTP 服务，19KB APK 内嵌在 flash |
| **静默安装 APK / 静默打开无障碍开关 / 静默授予读屏权** | ❌ 做不到，也不做 | Android 系统级安全边界：这几项必须由本人在屏幕上点一次确认。任何号称能绕过的方案本质上是在攻击手机，也正是用户一开始关心的风控红线 |
| **C 口线传控制数据** | ❌ 这块板不行 | ① 原生 USB 的 D-/D+ = GPIO19/20 与 ST7789 屏幕复用，之前实测开 USB 直接黑屏；② 小智大脑在云端，ESP32 本来就必须连 WiFi；③ Android 不向 USB 配件开放读屏 / 注入 UI 的能力，只剩盲按，满足不了「知道屏幕上有什么、装了什么 APP」 |

**结论**：那根 C 口线只负责给 ESP32 供电；控制数据走 WiFi。手机上唯一要装的东西被压缩到 **一个 19KB 的 APK + 一个系统开关**，而这个 APK 由 ESP32 自己托管下载、自动引导，不需要电脑、ADB、root、Termux、无线调试配对，也不用查 IP 或敲 `macset`。

---

## 2. 架构

```
   语音 ──► ESP32-S3（小智原固件：屏幕 / 唤醒 / WiFi / 云端大脑，全部保留）
                │
                ├─ MCP 工具 10 个（tools/list 上报云端）
                │     └─ PhoneBridgeActive() ?
                │          ├─ 是 → 固件内 phone_bridge.cc 直接编排（本方案）
                │          └─ 否 → 原 Portal :8080（Mac / Termux 老链路，仍可用）
                │
                ├─ HTTP 服务 :80  引导页 + /xiaozhi-eye.apk（19KB，flash rodata）
                ├─ 后台发现任务  记住引导页来访 IP / NVS 旧地址 → 探测 :7333
                │                  └─ 全新手机：每 30 秒扫一遍同网段 :7333（12 路并发）
                │                     找到后写入 NVS，重启免重配
                │
                └── WiFi ──► 手机 :7333  orb-eye 无障碍服务（一个 APK）
                                    └─ /screen /click /setText /swipe /home /back /info
                                         ↓ Android Accessibility API
                                       任意 APP 的真实控件树与操作
```

**为什么用无障碍服务而不是 ADB**：`AccessibilityService` 是 Android 给读屏软件用的官方 API，普通 APK 就能拿到完整控件树、按文字点击、中文 `ACTION_SET_TEXT`、手势滑动；不需要开发者选项、不需要 USB 调试、不需要 root、不需要电脑，也不会被「检测到 USB 调试」的金融类 App 拒绝。社区同类方案（AutoGLM / Open-AutoGLM / orb-eye / Droidline）都走这条路。

---

## 3. 一次性设置（约 2 分钟，只做一次）

1. **ESP32 上电**，串口日志会打印：
   ```
   ========== 手机接入引导页: http://10.197.1.100/ ==========
   ```
   （地址由路由器分配，以实际日志为准）

2. **手机连同一个 WiFi，用浏览器打开这个地址** → 页面上有「下载配套 App（19 KB）」按钮。
   点下载 → 按系统提示点「安装」。若提示禁止安装未知应用，在浏览器 / 文件管理器设置里允许一次。
   > 这一刻 ESP32 会尝试记住来访 IP；即使这一步没成功，后台还有局域网扫描兜底。

3. **打开开关**：手机 设置 → 无障碍 / 辅助功能 → 已下载的服务 → `Orb Eye` → 打开。
   > Android 13+ 若开关是灰的：应用详情 → 右上角 ⋮ → 允许受限设置，再回来开。

刷新引导页，状态变绿「✓ 已连接手机 192.168.x.x:7333」即可直接对小智说话。ESP32 每 2.5 秒探测一次记住的地址；**全新手机或换过路由器时**，后台每 30 秒用 12 路并发扫一遍同网段的 :7333，找到后自动把 10 个 MCP 工具切到固件直连模式，并把 IP 写进 NVS，重启免重配。

**备用命令**（只在自动发现没生效时用）：串口 `phonebind <手机IP>` 或浏览器打开 `http://<ESP32 IP>/bind?ip=<手机IP>`；`phonestatus` 查状态。换手机时 `phoneunbind` 或访问 `/unbind` 清除旧地址让它重新扫描。想切回控 Mac 老链路：`macset <MacIP>:8080 test`。

---

## 4. 固件里到底写了什么

`phone_bridge.cc` 是核心，全部在 ESP32 上运行：

- **协议桥**：把 10 条 Portal 风格路由翻译成 orb-eye 的原子 API。
- **真实控件树**：解析 `/screen` 的 `elements`（text / desc / bounds / clickable / editable），压成中文简要回传给云端，**不编造屏幕内容**。
- **动态应用发现**：回桌面 → 读图标文字 → 按名字点开 → 回读前台包名。找不到就翻桌面试 3 页，仍找不到就如实回报桌面上真实可见的名字。固件里没有任何 APP 名单或包名映射表。
- **中文整句编排**：`打开 X → 点搜索框 → 输入词 → 按回车 → 回读屏幕`，以及发消息、点歌、浏览器、上下滑、返回。每步后都读回真实屏幕文字。
- **敏感动作二次确认**：发消息 / 回复 / 下单先返回 8 位确认码，必须再说「确认执行 <码>」才真的点发送。支付 / 密码 / 验证码一律不自动执行。
- **去重**：本地 STT 兜底与云端 `tools/call` 对同一句话只执行一次（15 秒业务键去重）。
- **回车处理**：无障碍服务没有 keyevent，固件按界面上真实存在的「搜索 / 回车 / Enter / 确定」按钮点；自绘键盘读不到按键时如实说明，不假称已按。

| 固件 MCP 工具 | 手机直连模式下的行为 |
|---|---|
| `self.mac.list_apps` | 回桌面读真实图标名（翻 3 页） |
| `self.mac.open_app` | 回桌面按名字点图标，回读屏幕 |
| `self.mac.status` / `screen` | 当前应用 + 控件树中文简要 |
| `self.mac.click` / `input_text` | 坐标点击 / 中文 `ACTION_SET_TEXT` |
| `self.mac.send_message` | 整句编排 + 二次确认 |
| `self.mac.play_music` / `browser` | 打开对应应用 → 搜索 → 回车 → 回读 |
| `self.phone.control` | 整句 → 动作序列（同上） |

10 个工具名称、参数、云端 `tools/list` 全部不变，所以**不需要改小智后台配置，也不需要重新激活设备**。

---

## 5. 文件清单

| 文件 | 说明 |
|---|---|
| `main/boards/bread-compact-wifi-s3cam/phone_bridge.cc/.h` | 引导服务 + 自动发现 + 协议桥 + 中文编排（本方案核心） |
| `main/boards/bread-compact-wifi-s3cam/orb_eye_apk.h` | 19KB APK 编译进 rodata（flash，不占 SRAM） |
| `main/boards/bread-compact-wifi-s3cam/mac_control_tools.cc` | `Exec()` 增加桥模式直调，避免 HTTP 自环死锁 |
| `main/boards/bread-compact-wifi-s3cam/compact_wifi_board_s3cam.cc` | 网络就绪后启动引导服务 |
| `main/boards/bread-compact-wifi-s3cam/voice_selftest.cc` | 串口 `phonestatus` / `phonebind` |
| `main/CMakeLists.txt` | 增加 `esp_http_server` 组件依赖 |
| `tools/phone_apk/orb-eye.apk` | MIT 协议原版 APK，SHA256 `525c4f27…3824` |
| `tools/phone_apk/OrbAccessibilityService.java` | 上游源码，便于核对接口契约 |
| `tools/mock_orb_eye.py` | 模拟手机服务，用于无真机时端到端自测 |

APK 来源：`github.com/EatTake/orb-eye` v2.0，MIT。一个 Java 文件、一个无障碍服务、一个 HTTP server，绑定 `0.0.0.0:7333`，无账号、无云端上传。

---

## 6. 实测记录（2026-11-10）

- 编译 0 错误，固件 `0x298ca0` bytes，app 分区剩余 34%；烧录 5 分区 Hash verified。
- 启动稳定：无 assert、无反复重启；10 个 MCP 工具注册；云端 `tools/list` 分页 7812 + 3077 bytes；稳态空闲 SRAM 约 44KB、minimal 约 33KB，语音与屏幕正常。
- 引导页 `HTTP 200 / 2889 bytes`，中文完整；`/xiaozhi-eye.apk` 返回 18951 bytes 且 SHA256 与源文件完全一致。
- ESP32 打印 `http://10.197.1.100/`，自动发现到模拟手机 `10.197.1.38:7333` 后 `/status` 返回 `active:true`。
- 串口整句 `macphone 打开美团帮我找一下奶茶多少钱` → 固件编排产生真实动作序列：`/home → /screen → /tap 美团 → /click 搜索 → /setText → /click 回车`，模拟手机读回「喜茶 18 元 / 奈雪 22 元 / 蜜雪冰城 7 元」。
- 敏感动作闭环：`打开抖音回复：真不错` → 先给 8 位确认码不执行 → `确认执行 <码>` 后才打开抖音、输入、点发送 → 重放同一码被拒绝。
- 局域网自动扫描：`phoneunbind` 清空记住的 IP 后，从零状态约 0.8 秒扫到同网段手机并自动绑定，`/status` 回到 `active:true`；换新手机不需要任何手工配置。
- 修过的四个真实问题：① 在 WiFi 就绪前启动 httpd 会 `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)` 并反复重启 → 改为等拿到 IP 再启动；② `esp_http_client_read` 读完 `Content-Length` 仍等连接关闭导致每步空等 6 秒 → 改为读够就停；③ **IDF 的 `esp_http_client_open()` 只发请求头，POST body 必须由调用方 `esp_http_client_write()` 显式写出**——漏了这步导致每个 POST 在 `fetch_headers` 死等 6 秒超时（诊断手段：给每次请求加分阶段耗时日志，GET 25ms / POST 6012ms 一眼定位），补上后整句执行从 60 秒超时降到 **5.2 秒**；④ 探测超时 450ms 小于慢速 WiFi 的 ~700ms RTT，导致在线状态来回抖动 → 放宽到 2.5 秒。
- 每次手机请求耗时 17-37ms（`[eye]` 串口日志实时可见，真机排查直接看这个）。

**注意**：以上是用 `tools/mock_orb_eye.py` 模拟手机做的链路验证，**真机尚未接入**。厂商桌面 / 控件树 / 输入法可能与模拟环境不同，第一次实跑请看串口日志与 `http://<ESP32 IP>/status`。

---

## 7. 边界与合规（如实说明）

- **装 APK、开无障碍开关这两步必须本人点一次**，Android 强制，不提供任何绕过方式。
- orb-eye 的 `127.0.0.1:7333` 实际绑定 `0.0.0.0`，**没有鉴权**。请只在自己信任的 WiFi 下使用，不要暴露到公网；公共 WiFi 存在同网段他人可操作手机的风险。
- 自绘界面（游戏、部分视频 / 地图）无障碍树里没有文字，会如实回报「读不到」，不用 OCR 猜价格，也不编造结果。
- 锁屏 PIN / 图案 / 密码无法代替输入；支付、密码、验证码不自动执行；部分 App 会检测无障碍服务并限制功能，这是平台策略，不绕过。
- 发送 / 回复 / 下单必须语音二次确认；重复指令去重，避免把消息发两遍。
- iPhone 非越狱没有等价控制通道，本方案只覆盖 Android。
- 老链路（Mac Portal / Termux 自托管）仍然保留：`macset` 指定 Portal 地址即可；手机离线超过 20 秒会自动回退。

---

## 8. 排错

| 现象 | 处理 |
|---|---|
| 引导页打不开 | 看串口日志里的 IP；确认手机和 ESP32 在同一 WiFi（不是访客网络 / AP 隔离） |
| 状态一直「等待手机接入」 | 确认无障碍开关已开；`phonebind <手机IP>` 手动绑定；手机浏览器打开 `http://<手机IP>:7333/ping` 应返回 `orb-eye` |
| 打开应用总说找不到 | 固件按桌面图标中文名找，若厂商桌面把图标放在文件夹里，先手动把应用拖到桌面 |
| 按回车没反应 | 自绘键盘无障碍读不到按键，固件会如实说明；可改说「点搜索」 |
| 语音说「我控制不了手机」 | 工具描述已设备无关；确认 `phonestatus` 为 active，必要时重说一次 |
| 想切回控电脑 | 串口 `macset <MacIP>:8080 test` |
