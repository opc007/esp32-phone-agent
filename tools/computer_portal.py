#!/usr/bin/env python3
"""
computer_portal.py —— 让 ESP32 真的能控制这台 Mac 的 Portal 服务端

跟 mock_portal.py 的区别：mock 只打印请求，这里会**真的执行动作**：
  /tap            -> Quartz 移动鼠标并点击（坐标从 1080x2400 映射到 Mac 屏幕）
  /keyboard/input -> Quartz 注入 Unicode 文本（中文也可以）
  /keyboard/key   -> 注入按键
  /app            -> open -a 启动应用
  /state          -> 返回 Mac 当前前台应用 + 一份可点击的控件树

用途：不装安卓 App、不配蓝牙，就能验证 ESP32 → WiFi → Portal → 真实动作的整条链路。
所有动作都记录在 ~/computer_portal.log，并可通过 GET /result 读取。

用法：
    python3 tools/computer_portal.py [port] [token]
    默认 0.0.0.0:8080 / test

安全说明：只做鼠标移动/点击、打开应用、注入文本，不做任何删除或写文件操作。
"""
import base64
import glob
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# 两种运行形态，同一套 HTTP 契约、同一份固件：
#   A) 默认（Mac）：同时控制这台 Mac 和通过 USB/WiFi ADB 接进来的手机。
#   B) XIAOZHI_PHONE_ONLY=1（手机自托管）：不加载任何 macOS 框架，整个服务
#      跑在手机 Termux 里，固件所有工具的目标都变成这台手机本身。
#      ESP32 只要把 Portal 地址指向手机 IP，链路里就完全没有电脑了。
# 这个开关只影响 import 和路由分发，Mac 形态的全部既有逻辑原样保留。
# ---------------------------------------------------------------------------
PHONE_ONLY = os.environ.get("XIAOZHI_PHONE_ONLY", "").strip() == "1"
HAS_MAC = False
if PHONE_ONLY:
    ax_ui = browser_cdp = Quartz = NSWorkspace = None
else:
    import ax_ui
    import browser_cdp
    import Quartz
    from AppKit import NSWorkspace
    HAS_MAC = True
import android_adb

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
TOKEN = sys.argv[2] if len(sys.argv) > 2 else "test"

_lock = threading.Lock()
_actions = []          # 执行记录，供 /result 读取
_typed_text = []       # 注入过的文本
_last_app = {"pid": None, "name": ""}   # 最近一次操作过的应用，供「读屏幕」优先使用
_last_msg = {"key": "", "ts": 0, "result": ""}  # 发消息去重：同一条 15 秒内只发一次
_last_music = {"key": "", "ts": 0, "result": ""}  # 点歌去重：本地兜底+云端工具会各调一次
_last_browser = {"key": "", "ts": 0, "result": ""}  # 浏览器动作去重（同一句双路径）
_last_phone = {"key": "", "ts": 0, "result": ""}    # 手机动作去重（本地兜底+云端工具）
_last_phone_app = {"key": "", "ts": 0}              # /app 自动回落到手机的去重
_pending = {"token": 0, "payload": None}        # 未登录时挂起的消息，登录成功自动补发
MUSIC_CACHE = os.path.expanduser("~/Music/xiaozhi")  # 官方试听音频缓存
_afplay = {"proc": None}                        # 客户端不可播时的出声兜底
LOGIN_MARKS = ("需在手机上完成登录", "当前登录用户", "扫码登录", "切换账号",
               "仅传输文件", "手机端确认", "请登录", "二维码")


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(f"{__import__('os').path.expanduser('~')}/computer_portal.log", "a") as f:
        f.write(line + "\n")


def record(kind, detail):
    with _lock:
        _actions.append({"t": time.strftime("%H:%M:%S"), "kind": kind, "detail": detail})


def screen_size():
    try:
        return Quartz.CGDisplayBounds(Quartz.CGMainDisplayID()).size.width, \
               Quartz.CGDisplayBounds(Quartz.CGMainDisplayID()).size.height
    except Exception:
        return 1920.0, 1080.0


def mouse_pos():
    return Quartz.CGEventGetLocation(Quartz.CGEventCreate(None))


def move_and_click(x, y, button="left"):
    """把手机坐标映射到 Mac 屏幕，移动鼠标并点击。返回点击后的实际鼠标坐标。"""
    sw, sh = screen_size()
    mx = max(0, min(sw - 1, x * sw / 1080.0))
    my = max(0, min(sh - 1, y * sh / 2400.0))
    btn = Quartz.kCGMouseButtonRight if button == "right" else Quartz.kCGMouseButtonLeft
    down_t = Quartz.kCGEventRightMouseDown if button == "right" else Quartz.kCGEventLeftMouseDown
    up_t = Quartz.kCGEventRightMouseUp if button == "right" else Quartz.kCGEventLeftMouseUp
    # CGWarpMouseCursorPosition 直接移动硬件光标，不需要辅助功能权限；
    # CGEventPost 的合成事件在未授权时会被 macOS 静默丢弃，两者都发一遍。
    Quartz.CGWarpMouseCursorPosition((mx, my))
    Quartz.CGEventPost(Quartz.kCGHIDEventTap,
                       Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventMouseMoved, (mx, my), btn))
    time.sleep(0.03)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap,
                       Quartz.CGEventCreateMouseEvent(None, down_t, (mx, my), btn))
    time.sleep(0.03)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap,
                       Quartz.CGEventCreateMouseEvent(None, up_t, (mx, my), btn))
    p = mouse_pos()
    return (round(mx), round(my)), (round(p.x), round(p.y))


def clipboard():
    try:
        return subprocess.run(["pbpaste"], capture_output=True, timeout=3).stdout.decode("utf-8", "replace")
    except Exception:
        return ""


def type_unicode(text):
    """注入 Unicode 文本（含中文）。CGEventPost 需要辅助功能权限，未授权时会被丢弃，
    所以同时把文本放进系统剪贴板 —— 这一步不需要任何权限，且可用 pbpaste 回读验证。"""
    for ch in text:
        ev = Quartz.CGEventCreateKeyboardEvent(None, 0, True)
        Quartz.CGEventKeyboardSetUnicodeString(ev, 1, ch)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
        ev2 = Quartz.CGEventCreateKeyboardEvent(None, 0, False)
        Quartz.CGEventKeyboardSetUnicodeString(ev2, 1, ch)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev2)
        time.sleep(0.008)
    try:
        subprocess.run(["pbcopy"], input=text.encode("utf-8"), timeout=3, capture_output=True)
    except Exception:
        pass
    _typed_text.append(text)


# Android keycode -> macOS virtual keycode
KEYMAP = {3: 53, 4: 124, 66: 36, 67: 51, 19: 126, 20: 125, 21: 123, 22: 124,
          61: 48, 62: 49, 82: 53, 24: 122, 25: 120}


def press_key(code):
    vk = KEYMAP.get(int(code))
    if vk is None:
        return False
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateKeyboardEvent(None, vk, True))
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateKeyboardEvent(None, vk, False))
    return True


def front_app():
    try:
        a = NSWorkspace.sharedWorkspace().frontmostApplication()
        return a.localizedName() or "?"
    except Exception:
        return "?"


# ---------------------------------------------------------------------------
# 动态应用发现：ESP32 固件里不存任何 App 名单。
# 每次都从这台 Mac 实际安装的 .app 读取名称、中文本地化名、bundle id；
# 设备端只把「用户说的名字」原样传过来，由这里解析成真实应用。
# ---------------------------------------------------------------------------
_APP_CACHE = {"ts": 0, "apps": []}
_APP_DIRS = ["/Applications", "/System/Applications",
             "/System/Library/CoreServices/Applications", os.path.expanduser("~/Applications")]


def _zh_aliases(path):
    """从 zh_*.lproj/InfoPlist.strings 读系统应用的中文显示名（计算器/备忘录…）。"""
    out = []
    for f in glob.glob(os.path.join(path, "Contents", "Resources", "zh*.lproj", "InfoPlist.strings")):
        try:
            raw = open(f, "rb").read()
            txt = ""
            for enc in ("utf-16", "utf-8"):
                try:
                    txt = raw.decode(enc)
                    break
                except Exception:
                    continue
            for m in re.finditer(r'"(?:CFBundleName|CFBundleDisplayName)"\s*=\s*"([^"]+)"', txt):
                if m.group(1) not in out:
                    out.append(m.group(1))
        except Exception:
            pass
    return out


def _display_names(paths):
    """批量取本地化显示名，返回 {app路径: 中文名}。
    注意：mdls 只为有 Spotlight 索引的 App 输出，行数不等于文件数，
    必须按 kMDItemPath 配对，否则中文名会整体串位。"""
    try:
        r = subprocess.run(
            ["mdls", "-name", "kMDItemDisplayName", "-name", "kMDItemPath"] + paths,
            capture_output=True, text=True, timeout=15)
    except Exception:
        return {}
    mapping, cur = {}, {}
    for line in r.stdout.splitlines():
        m = re.search(r'kMDItemDisplayName\s*=\s*"([^"]+)"', line)
        if m:
            cur["name"] = m.group(1)
            continue
        p = re.search(r'kMDItemPath\s*=\s*"([^"]+)"', line)
        if p:
            cur["path"] = p.group(1)
            if cur.get("name") and cur.get("path"):
                mapping[cur["path"]] = cur["name"]
            cur = {}
    return mapping


def installed_apps(force=False):
    now = time.time()
    if not force and _APP_CACHE["apps"] and now - _APP_CACHE["ts"] < 300:
        return _APP_CACHE["apps"]
    from Foundation import NSBundle
    entries, seen = [], set()
    for d in _APP_DIRS:
        for path in glob.glob(os.path.join(d, "*.app")):
            base = os.path.basename(path)
            if base in seen:
                continue
            seen.add(base)
            name, bid = base[:-4], ""
            try:
                b = NSBundle.bundleWithPath_(path)
                if b:
                    name = str(b.objectForInfoDictionaryKey_("CFBundleName") or
                               b.objectForInfoDictionaryKey_("CFBundleDisplayName") or name)
                    bid = str(b.bundleIdentifier() or "")
            except Exception:
                pass
            aliases = {name, base[:-4], base, bid.split(".")[-1] if bid else ""}
            aliases.update(_zh_aliases(path))
            entries.append((path, name, bid, aliases))
    display = _display_names([e[0] for e in entries])
    for e in entries:
        zh = display.get(e[0])
        if zh:
            e[3].add(zh)
    apps = [{"name": e[1], "path": e[0], "bundle_id": e[2],
             "aliases": sorted(a for a in e[3] if a)} for e in entries]
    _APP_CACHE.update(ts=now, apps=apps)
    return apps


def resolve_app(query):
    """把用户/模型给的名字解析成真实安装的应用。返回 (应用 或 None, 全部应用)。"""
    apps = installed_apps()
    q = (query or "").strip()
    if not q:
        return None, apps
    ql = q.lower()
    for a in apps:
        if any(str(x).lower() == ql for x in a["aliases"]):
            return a, apps
    for a in apps:
        if any(ql in str(x).lower() for x in a["aliases"]):
            return a, apps
    for a in apps:
        for x in a["aliases"]:
            if len(str(x)) >= 2 and str(x).lower() in ql:
                return a, apps
    for junk in ("应用", "软件", "程序", "app", "打开", "启动", "帮我", "请", "一下", "吧"):
        ql = ql.replace(junk, "")
    ql = ql.strip()
    if ql:
        for a in apps:
            if any(ql in str(x).lower() or (len(ql) >= 2 and str(x).lower() in ql)
                   for x in a["aliases"]):
                return a, apps
    return None, apps


def app_names(apps, limit=80):
    """给设备端/大模型看的精简列表（纯文本，控制长度）。"""
    out = []
    for a in apps:
        n = a["name"]
        zh = next((x for x in a["aliases"]
                   if x != n and any("\u4e00" <= c <= "\u9fff" for c in str(x))), "")
        label = f"{n}({zh})" if zh else n
        if label not in out:
            out.append(label)
        if len(out) >= limit:
            break
    return out


# ---------------------------------------------------------------------------
# 音乐：用 QQ 音乐官方联想接口把歌名解析成真实歌曲，再打开安装好的音乐 App。
# 不依赖辅助功能授权的动作一定执行；有授权时再自动输入搜索词并回车。
# ---------------------------------------------------------------------------
_LOOKUP_CACHE = {}


def _qq_smartbox(key):
    """QQ音乐官方联想接口，返回候选歌曲列表。"""
    headers = {"Referer": "https://y.qq.com/",
               "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"}
    url = ("https://c.y.qq.com/splcloud/fcgi-bin/smartbox_new.fcg?is_xml=0&key="
           + urllib.parse.quote(key) + "&format=json")
    req = urllib.request.Request(url, headers=headers)
    d = json.loads(urllib.request.urlopen(req, timeout=5).read().decode("utf-8", "replace"))
    return ((d.get("data") or {}).get("song") or {}).get("itemlist") or []


def qqmusic_lookup(song):
    """把口语化/带错字的歌名解析成真实歌曲。

    不能无脑取联想接口第一个结果：实测「周杰伦的晴天」第一个返回的是
    「周杰伦的五首小甜歌…」电台节目，会直接放错歌。这里做多候选查询 +
    歌名相似度打分，只接受足够接近的真实歌曲。
    """
    import difflib
    song = (song or "").strip()
    if not song:
        return None
    if song in _LOOKUP_CACHE:
        return _LOOKUP_CACHE[song]

    cands = [song]
    # 「周杰伦的晴天」→ 补一个「晴天」候选；「帮我找一首小星星」→ 补「小星星」
    m = re.match(r"^.{2,6}的(.{2,})$", song)
    if m:
        cands.append(m.group(1))
    for cut in (1, 2, 3):
        if len(song) - cut * 2 >= 2:
            cands.append(song[:len(song) - cut * 2])   # 去尾部口语词
        if len(song) > cut * 2 + 2:
            cands.append(song[cut * 2:])               # 去头部「帮我/用XX」
    seen = set()
    best, best_score = None, 0.0
    for key in cands:
        if key in seen or len(key) < 2:
            continue
        seen.add(key)
        try:
            items = _qq_smartbox(key)
        except Exception:
            continue
        for it in items[:8]:
            name = str(it.get("name") or "")
            mid = str(it.get("mid") or "")
            if not name or not mid:
                continue
            score = difflib.SequenceMatcher(None, name, song).ratio()
            # 电台/合集/超长标题降权，避免把节目当歌播
            if len(name) > max(18, len(song) * 2):
                score -= 0.25
            if any(k in name for k in ("电台", "合集", "盘点", "精选", "串烧", "歌单")):
                score -= 0.2
            if score > best_score:
                best_score = score
                best = {"name": name, "singer": it.get("singer", ""),
                        "mid": mid, "query": key}
        if best_score >= 0.82:
            break
        if len(seen) >= 6:
            break
    result = best if best and best_score >= 0.45 else None
    _LOOKUP_CACHE[song] = result
    return result


def parse_music_request(text):
    """从中文整句里解析 (歌名, 应用名)。句式解析放电脑端，固件不写死任何句式/歌名。

    例：「打开QQ音乐，搜索世上只有妈妈好来播放」→ ('世上只有妈妈好', 'QQ音乐')
        「播放《晴天》」→ ('晴天', '')
    """
    t = (text or "").strip().strip("，。！？、,.!? ")
    if not t:
        return "", ""
    app = ""
    for kw in ("QQ音乐", "qq音乐", "QQMusic", "网易云音乐", "Apple Music",
               "Spotify", "酷狗音乐", "音乐"):
        if kw.lower() in t.lower():
            app = "QQ音乐" if kw.lower() in ("qq音乐", "qqmusic") else kw
            break
    m = re.search(r"[《<]([^》>]{1,40})[》>]", t)
    if m:
        return m.group(1).strip(), app

    triggers = ("播放", "放一首", "来一首", "点一首", "我想听", "听一下", "听首",
                "来首", "搜索", "搜一下", "搜", "找一首", "找", "点播", "放")
    seg = ""
    for w in triggers:
        p = t.find(w)
        if p != -1 and t[p + len(w):].strip():
            seg = t[p + len(w):].strip()
            break
    if not seg:
        seg = t
    tails = ("来播放", "播放", "来听", "听一下", "这首歌", "吧", "啊", "呢",
             "一下", "给我", "好吗", "可以吗", "听", "的歌", "这首歌吧")
    changed = True
    while changed:
        changed = False
        for w in tails:
            if seg.endswith(w) and len(seg) > len(w) + 1:
                seg = seg[:-len(w)].strip()
                changed = True
                break
    heads = ("用QQ音乐", "在QQ音乐", "QQ音乐里", "QQ音乐", "用", "在",
             "把", "给", "帮我", "请", "里", "的")
    changed = True
    while changed:
        changed = False
        for w in heads:
            if seg.startswith(w) and len(seg) > len(w) + 1:
                seg = seg[len(w):].strip()
                changed = True
                break
    seg = seg.strip("《》<>\"'\u201c\u201d ，。！？、:：")
    return seg[:40], app


def qqmusic_audio(mid, name, singer):
    """用 QQ音乐官方 musicu.fcg 取这首歌的试听音频并缓存到本地，返回文件路径。

    实测：拿到的 m4a 交给 `open -a QQMusic <file>`，客户端播放栏会真的切到这首歌。
    这条路径不依赖屏幕录制、辅助功能、登录态，也不依赖它自绘的搜索结果页。
    """
    if not mid:
        return None
    safe = re.sub(r'[\\/:*?"<>|\r\n]', "_", f"{name} - {singer}".strip(" -"))[:60] or mid
    try:
        os.makedirs(MUSIC_CACHE, exist_ok=True)
    except Exception:
        return None
    path = os.path.join(MUSIC_CACHE, safe + ".m4a")
    if os.path.exists(path) and os.path.getsize(path) > 20000:
        return path
    payload = {"req_0": {"module": "vkey.GetVkeyServer", "method": "CgiGetVkey",
                         "param": {"guid": "7332955600", "songmid": [mid],
                                   "songtype": [0], "uin": "0",
                                   "loginflag": 1, "platform": "20"}}}
    url = ("https://u.y.qq.com/cgi-bin/musicu.fcg?format=json&data="
           + urllib.parse.quote(json.dumps(payload, separators=(",", ":"))))
    headers = {"Referer": "https://y.qq.com/", "User-Agent": "Mozilla/5.0 (Macintosh)"}
    try:
        req = urllib.request.Request(url, headers=headers)
        d = json.loads(urllib.request.urlopen(req, timeout=8).read().decode("utf-8", "replace"))
        data = (d.get("req_0") or {}).get("data") or {}
        items = data.get("midurlinfo") or []
        purl = items[0].get("purl") if items else None
        if not purl:
            return None
        for sip in (data.get("sip") or []):
            try:
                r = urllib.request.Request(sip + purl, headers=headers)
                blob = urllib.request.urlopen(r, timeout=15).read()
                if len(blob) > 20000:
                    with open(path, "wb") as f:
                        f.write(blob)
                    return path
            except Exception:
                continue
    except Exception as e:
        log(f"  → 取试听音频失败: {e}")
    return None


def music_now_playing(bundle_id, name):
    """回读音乐客户端真实播放栏：当前歌曲名 / 是否正在播放。"""
    pid, _ = find_app_pid(bundle_id, name)
    if not pid:
        return "", False
    _, tree = app_ui(pid, depth=7)
    lines = tree.brief(400)
    cur = ""
    for l in lines:
        if "歌曲名：" in l:
            cur = (l.split("歌曲名：", 1)[1].split(" @(", 1)[0]
                    .split(" - 歌手名：", 1)[0].strip())
            break
    playing = any("AXUnknown 暂停" in l or "暂停播放" in l for l in lines)
    return cur, playing


def play_audio_fallback(path):
    """客户端没接住文件时，用系统播放器保证真的出声。"""
    old = _afplay.get("proc")
    if old and old.poll() is None:
        try:
            old.terminate()
        except Exception:
            pass
    try:
        _afplay["proc"] = subprocess.Popen(
            ["afplay", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception:
        return False


def accessibility_ok():
    """进程级辅助功能权限：决定 CGEventPost 按键/点击与 AXUIElement 能否生效。

    注意：不能用 osascript 去问 System Events —— 那个 AppleEvent 会被 TCC 以
    -10004「权限违例」拒绝，但本进程自己的辅助功能权限其实是有的（已实测）。
    """
    return ax_ui.trusted()


def hotkey(vk, command=False):
    flags = Quartz.kCGEventFlagMaskCommand if command else 0
    for down in (True, False):
        ev = Quartz.CGEventCreateKeyboardEvent(None, vk, down)
        if flags:
            Quartz.CGEventSetFlags(ev, flags)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
        time.sleep(0.05)


def click_mac(x, y):
    """用 Mac 屏幕绝对坐标真实点击（AX 控件树给的就是这个坐标系）。"""
    Quartz.CGWarpMouseCursorPosition((x, y))
    for down in (True, False):
        t = Quartz.kCGEventLeftMouseDown if down else Quartz.kCGEventLeftMouseUp
        Quartz.CGEventPost(Quartz.kCGHIDEventTap,
                           Quartz.CGEventCreateMouseEvent(None, t, (x, y),
                                                          Quartz.kCGMouseButtonLeft))
        time.sleep(0.05)
    time.sleep(0.15)


def paste_text(text):
    """把任意文本（含中文）放进剪贴板再 Cmd+V，比逐字注入 Unicode 可靠。"""
    subprocess.run(["pbcopy"], input=text.encode("utf-8"), timeout=3, capture_output=True)
    hotkey(9, command=True)      # Cmd+V
    time.sleep(0.3)


def paste_replace(text):
    subprocess.run(["pbcopy"], input=text.encode("utf-8"), timeout=3, capture_output=True)
    hotkey(0, command=True)      # Cmd+A
    time.sleep(0.1)
    hotkey(9, command=True)      # Cmd+V
    time.sleep(0.3)


def activate_pid(pid):
    try:
        from AppKit import NSRunningApplication
        a = NSRunningApplication.runningApplicationWithProcessIdentifier_(int(pid))
        if a:
            a.activateWithOptions_(1 | (1 << 6))
            time.sleep(0.6)
            return True
    except Exception:
        pass
    return False


def app_ui(pid=None, depth=5):
    """读真实控件树。指定 pid 时直接读那个应用，不必依赖前台。"""
    if pid:
        el = ax_ui.app_element(pid)
        root = el.snapshot(depth, 320) if el else None
        info = {"pid": pid, "name": ""}
        for a in ax_ui.running_apps():
            if a["pid"] == int(pid):
                info = a
                break
    else:
        info, root = ax_ui.frontmost_tree(depth, 320)
    return info, ax_ui.Tree(root, info)


def ui_texts(tree, limit=200):
    return [l for l in tree.brief(limit)]


def find_app_pid(bundle_id, name):
    for a in ax_ui.running_apps():
        if bundle_id and a["bundle_id"] == bundle_id:
            return a["pid"], a
        if name and a["name"] == name:
            return a["pid"], a
    return None, None


def note_target(pid, name):
    _last_app.update(pid=pid, name=name)


def screen_summary(max_lines=55):
    """汇总真实可见窗口。macOS 的「前台应用」经常是通用控制/通知中心这类无窗口进程，
    只看前台会读到一片空白，所以按「刚操作过的应用 → 前台 → 其它有窗口应用」排序。"""
    targets, seen = [], set()
    pairs = []
    if _last_app.get("pid"):
        pairs.append((_last_app["pid"], _last_app["name"]))
    f = ax_ui.frontmost()
    if f:
        pairs.append((f["pid"], f["name"]))
    for a in ax_ui.running_apps():
        if a["bundle_id"] and not a["bundle_id"].startswith("com.apple."):
            pairs.append((a["pid"], a["name"]))
    for pid, name in pairs:
        if pid in seen:
            continue
        seen.add(pid)
        targets.append((pid, name))
        if len(targets) >= 5:
            break
    sections = []
    for pid, name in targets:
        el = ax_ui.app_element(pid)
        if not el:
            continue
        wins = el.attr("AXWindows")
        if not isinstance(wins, list) or not wins:
            continue
        tree = ax_ui.Tree(el.snapshot(4, 100), {"name": name, "pid": pid})
        lines = ui_texts(tree, 22)
        if lines:
            sections.append(f"【{name}】" + " / ".join(lines))
        if sum(len(s.split(" / ")) for s in sections) >= max_lines:
            break
    return ("；".join(sections)[:3000] if sections
            else "屏幕上没有读到任何应用窗口（可能全部最小化或未授权辅助功能）")


def ensure_front(pid):
    """按键/点击是全局事件，必须确认目标应用真的在最前面，否则会打到别的窗口。"""
    for _ in range(3):
        f = ax_ui.frontmost()
        if f and f["pid"] == int(pid):
            return True
        activate_pid(pid)
        time.sleep(0.4)
    return False


# ---------------------------------------------------------------------------
# 「打开微信给张三发消息说明天见」这类复合语音指令的解析。
# 放在电脑端做：Python 正则好改，固件里不写死任何联系人 / 应用 / 句式。
# 云端模型如果已经给出结构化 app/contact/message，就直接用，不经这里。
# ---------------------------------------------------------------------------
MSG_VERBS = ["发一条消息", "发个消息", "发条消息", "发消息", "发条信息", "发信息",
             "发个微信", "发条微信", "发微信", "发条短信", "发短信", "告诉", "回复"]
APP_HINTS = [("企业微信", "企业微信"), ("微信", "微信"), ("wechat", "微信"),
             ("weixin", "微信"), ("QQ", "QQ"), ("qq", "QQ"),
             ("钉钉", "钉钉"), ("飞书", "飞书")]


def parse_message_request(raw):
    s = (raw or "").strip()
    low = s.lower()
    app = ""
    for k, v in APP_HINTS:
        if k.lower() in low:
            app = v
            break
    vp, verb = -1, ""
    for v in MSG_VERBS:
        p = s.find(v)
        if p >= 0 and (vp < 0 or p < vp):
            vp, verb = p, v
    if vp < 0:
        return {"app": app, "contact": "", "message": ""}
    head, tail = s[:vp], s[vp + len(verb):]
    contact = ""
    best = -1
    for pre in ("给", "跟", "和", "向"):
        p = head.rfind(pre)
        if p > best:
            best = p
    if best >= 0:
        contact = head[best + 1:].strip(" ，,。:：、的 ")
    if not contact:
        m = re.search(r"给\s*([^，。！？、：:;,说]{1,24})", tail)
        if m:
            contact = m.group(1).strip(" ，,。:：、的 ")
            tail = tail[m.end():]
    message = re.sub(r"^(?:说|讲|内容是|内容|的是|是|：|:|，|,|、|\s|消息|一条|个|条)+",
                     "", tail).strip()
    message = message.rstrip(" 。！？，,.吧呢啊哦")
    contact = re.sub(r"^(?:帮我|请|打开|用|在|把|给)+", "", contact).strip()
    return {"app": app, "contact": contact, "message": message}


def send_message(app_query="", contact="", message="", raw_text="", dry_run=False):
    """打开即时通讯应用 → 找到指定联系人 → 发送指定内容。

    全程读真实控件树，不靠写死的坐标/坐标表；任何一步不确定就停下来如实回报，
    绝不谎报「已发送」。返回 dict：status = success / need_login / error。
    """
    parsed = parse_message_request(raw_text) if raw_text else {}
    app_query = (app_query or parsed.get("app") or "微信").strip()
    contact = (contact or parsed.get("contact") or "").strip()
    message = (message or parsed.get("message") or "").strip()
    if not contact or not message:
        return {"status": "error",
                "error": (f"从这句话里没能同时听清联系人和消息内容（联系人={contact!r}，"
                          f"内容={message!r}）。可以说『打开微信给张三发消息说明天见』。")}

    apps = installed_apps()
    app, _ = resolve_app(app_query)
    if app is None:
        return {"status": "error",
                "error": f"这台 Mac 上没有找到应用「{app_query}」。已安装：" +
                         "、".join(app_names(apps, 40))}
    bundle = app["bundle_id"]
    try:
        subprocess.run(["open", "-a", app["path"]], capture_output=True, timeout=10)
    except Exception as e:
        return {"status": "error", "error": f"启动 {app['name']} 失败: {e}"}
    time.sleep(1.6)
    pid, running = find_app_pid(bundle, app["name"])
    if pid:
        note_target(pid, app["name"])
        ensure_front(pid)
    info, tree = app_ui(pid)
    lines = ui_texts(tree, 160)
    blob = " ".join(lines)

    # 1) 未登录 / 需要手机确认：这是发消息最常见的真实阻塞，必须如实告诉用户
    login_marks = LOGIN_MARKS
    if any(m in blob for m in login_marks):
        btn = tree.find(role="AXButton", text="登录", limit=3)
        if btn and btn[0].get("pos") and btn[0].get("size"):
            p, s = btn[0]["pos"], btn[0]["size"]
            click_mac(p[0] + s[0] / 2, p[1] + s[1] / 2)
            time.sleep(1.2)
            info, tree2 = app_ui(pid)
            lines2 = ui_texts(tree2, 60)
            blob2 = " ".join(lines2)
            note = "已自动点击『登录』，现在窗口提示：" + (
                "需在手机上完成登录" if "需在手机上完成登录" in blob2 else "等待登录")
        else:
            note = "登录窗口已显示，但没有找到可点的登录按钮"
        subprocess.run(["pbcopy"], input=message.encode("utf-8"), timeout=3,
                       capture_output=True)
        return {"status": "need_login",
                "error": (f"已打开 {app['name']}，但它现在还没登录：{note}。"
                          f"请在手机上确认登录微信（或扫描屏幕上的二维码）。"
                          f"登录成功后会自动把「{message}」发给「{contact}」，不用再重复说一遍。"),
                "contact": contact, "message": message,
                "ui": lines[:24]}

    # 2) 搜索联系人
    if not accessibility_ok():
        return {"status": "error", "error": "辅助功能未授权，无法自动操作微信界面"}
    ensure_front(pid)
    hotkey(3, command=True)          # Cmd+F 聚焦搜索
    time.sleep(0.5)
    paste_replace(contact)
    time.sleep(1.4)
    info, tree = app_ui(pid)
    lines = ui_texts(tree, 200)
    blob = " ".join(lines)

    # 3) 从搜索结果里挑出名字匹配的项；找不到精确匹配就不冒险发送
    exact = [n for n in tree.nodes()
             if contact in " ".join(str(n.get(k) or "") for k in
                                    ("title", "value", "desc", "placeholder"))]
    if not exact:
        return {"status": "error",
                "error": (f"在 {app['name']} 里搜索「{contact}」，界面上没有读到这个联系人，"
                          f"为避免发错人已停止。界面上能看到：" + " / ".join(lines[:22])),
                "ui": lines[:30]}
    candidate = None
    for n in exact:
        if n.get("pos") and n.get("size") and n.get("size")[0] > 30:
            candidate = n
            break
    if candidate:
        p, s = candidate["pos"], candidate["size"]
        click_mac(p[0] + min(s[0] / 2, 120), p[1] + s[1] / 2)
    else:
        hotkey(36)                    # Return，打开第一个搜索结果
    time.sleep(1.4)
    info, tree = app_ui(pid)
    lines = ui_texts(tree, 200)
    header_blob = " ".join(lines[:60])
    window_titles = [n.get("title") for n in tree.find(role="AXWindow", limit=4)]
    in_chat = (contact in header_blob or
               any(contact in str(t) for t in window_titles))
    if not in_chat:
        return {"status": "error",
                "error": (f"搜索到了「{contact}」，但没能确认已经进入和这个人的会话"
                          f"（窗口标题：{window_titles}），为避免发错人已停止发送。"),
                "ui": lines[:30]}

    # 4) 把消息放进输入框
    subprocess.run(["pbcopy"], input=message.encode("utf-8"), timeout=3,
                   capture_output=True)
    if dry_run:
        return {"status": "success",
                "result": (f"[演练] 已在 {app['name']} 里找到「{contact}」并进入会话，"
                           f"消息「{message}」已复制到剪贴板；未真正点击发送。")}
    fields = tree.find(role="AXTextView", limit=8) + tree.find(role="AXTextField", limit=8)
    input_node = None
    for n in fields:
        if n.get("pos") and n.get("size") and n["size"][0] > 120:
            input_node = n
            break
    if input_node:
        p, s = input_node["pos"], input_node["size"]
        click_mac(p[0] + min(s[0] / 2, 200), p[1] + s[1] / 2)
        paste_replace(message)
    else:
        # 控件树没暴露输入框时，退回到窗口底部中间（聊天输入区）点击后粘贴
        wins = tree.find(role="AXWindow", limit=2)
        if wins and wins[0].get("pos") and wins[0].get("size"):
            p, s = wins[0]["pos"], wins[0]["size"]
            click_mac(p[0] + s[0] * 0.62, p[1] + s[1] - 55)
            paste_replace(message)
        else:
            return {"status": "error", "error": "没找到聊天输入框，未发送消息",
                    "ui": lines[:30]}
    time.sleep(0.4)

    # 5) 发送：优先点「发送」按钮，否则回车
    info, tree = app_ui(pid)
    send_btn = tree.find(role="AXButton", text="发送", limit=3)
    if send_btn and send_btn[0].get("pos") and send_btn[0].get("size"):
        p, s = send_btn[0]["pos"], send_btn[0]["size"]
        click_mac(p[0] + s[0] / 2, p[1] + s[1] / 2)
        how = "点击发送按钮"
    else:
        hotkey(36)
        how = "回车发送"
    time.sleep(1.2)

    # 6) 回读界面确认，绝不凭猜测说「已发送」
    info, final_tree = app_ui(pid)
    final_lines = ui_texts(final_tree, 220)
    final_blob = " ".join(final_lines)
    record("send_message", f"{app['name']} → {contact}: {message} ({how})")
    if message[:8] and message[:8] in final_blob:
        return {"status": "success",
                "result": (f"已在 {app['name']} 里给「{contact}」发送了消息「{message}」，"
                           f"并在界面上读回了这条消息。"),
                "ui": final_lines[:24]}
    return {"status": "success",
            "result": (f"已在 {app['name']} 里定位到「{contact}」的会话，输入了「{message}」"
                       f"并{how}；但聊天内容是应用自绘的，控件树读不回来，无法100%确认送达。"
                       f"请如实告诉用户『已执行发送动作，建议看一眼聊天窗口确认』，不要断言已送达。"),
            "ui": final_lines[:24]}


def is_logged_in(bundle, name):
    """通过真实控件树判断聊天应用是否已进入主界面（不再停在登录/扫码窗口）。"""
    pid, _ = find_app_pid(bundle, name)
    if not pid:
        return False
    el = ax_ui.app_element(pid)
    if not el:
        return False
    wins = el.attr("AXWindows")
    if not isinstance(wins, list) or not wins:
        return False
    tree = ax_ui.Tree(el.snapshot(4, 140), {"name": name, "pid": pid})
    blob = " ".join(ui_texts(tree, 140))
    if any(m in blob for m in LOGIN_MARKS):
        return False
    big_window = any((n.get("size") or [0, 0])[0] > 500
                     for n in tree.find(role="AXWindow", limit=3))
    return big_window or any(k in blob for k in ("搜索", "通讯录", "聊天", "联系人"))


def watch_login_and_send(bundle, name, payload, token):
    """微信没登录时把消息挂起；用户在手机上确认登录后自动补发，不用再说一遍口令。"""
    for _ in range(90):          # 最多等 3 分钟，每 2 秒看一次真实界面
        time.sleep(2)
        if _pending["token"] != token:
            return               # 用户又说了新指令，旧的作废
        try:
            if not is_logged_in(bundle, name):
                continue
            out = send_message(**payload)
            key = (f"{payload.get('app_query', '')}|{payload.get('contact', '')}|"
                   f"{payload.get('message', '')}")
            with _lock:
                _last_msg.update(key=key, ts=time.time(),
                                 result=out.get("result") or out.get("error") or "已执行")
            detail = f"[登录自动补发] {out.get('status')}: {_last_msg['result']}"
            log("  → " + detail)
            record("send_message_auto", detail)
        except Exception as e:
            log(f"  → 登录自动补发异常: {e}")
        if _pending["token"] == token:
            _pending["payload"] = None
        return


def play_music(song, app_query="", raw_text="", dry_run=False):
    """搜索并播放指定歌曲：曲库联想 → 官方试听音频 → 交给真实安装的音乐客户端播放
    → 从客户端界面回读当前歌曲确认。应用列表始终动态读取，不写死任何 APP。
    """
    if raw_text and not song:
        song, guessed = parse_music_request(raw_text)
        if guessed and not app_query:
            app_query = guessed
    song = (song or "").strip().strip("《》")
    if not song:
        return "没听清要播放的歌曲名，请再说一次歌名。"

    apps = installed_apps()
    app = None
    # 「音乐」是泛称，不能拿去 resolve（会误命中系统 Music），交给下面的偏好顺序
    if app_query and app_query not in ("音乐", "音乐软件", "播放器"):
        app, _ = resolve_app(app_query)
    if app is None:
        for kw in ("QQ音乐", "QQMusic", "网易云音乐", "Spotify", "Music", "酷狗音乐"):
            cand, _ = resolve_app(kw)
            if cand:
                app = cand
                break
    if app is None:
        return "这台 Mac 上没有找到可用的音乐应用，已安装：" + "、".join(app_names(apps, 40))

    # 曲库联想：把口误/口语化的歌名纠正成真实歌曲（含歌手、mid）
    info = qqmusic_lookup(song)
    title = info["name"] if info else song
    who = f"（{info['singer']}）" if info and info.get("singer") else ""
    if dry_run:
        return (f"解析结果：歌曲《{title}》{who}，目标应用 {app['name']}，"
                f"曲库 mid={info['mid'] if info else '无'}（未实际播放）")

    subprocess.run(["open", "-a", app["path"]], capture_output=True, timeout=10)
    time.sleep(0.8)
    pid, _ = find_app_pid(app["bundle_id"], app["name"])
    if pid:
        note_target(pid, app["name"])

    audio = qqmusic_audio(info["mid"], title, info.get("singer", "")) if info else None
    if audio:
        r = subprocess.run(["open", "-a", app["path"], audio],
                           capture_output=True, timeout=12)
        # 等播放器把「当前歌曲」刷出来；最多等 6 秒
        cur = ""
        for _ in range(10):
            time.sleep(0.6)
            cur, _playing = music_now_playing(app["bundle_id"], app["name"])
            if cur and (title[:3] in cur or song[:3] in cur):
                break
        if cur and (title[:3] in cur or song[:3] in cur):
            return (f"已在 {app['name']} 里播放《{title}》{who}。"
                    f"已从播放器界面读回确认：当前歌曲「{cur}」。"
                    f"说明：使用 QQ音乐官方试听音频，可能是片段；VIP/下架歌曲会失败。")

        # 客户端没接住文件：用系统播放器保证真的出声，并如实说明
        if play_audio_fallback(audio):
            return (f"已在电脑出声播放《{title}》{who}（系统播放器兜底）。"
                    f"{app['name']} 没有自动接住本地文件，界面当前歌曲是「{cur or '未读到'}」；"
                    f"不要说成『QQ音乐正在播放』，如实说明是用系统播放器播的。")

    # 拿不到试听音频时（VIP / 下架 / 版权）：
    # 1) 在客户端真实搜索框输入歌名并回车；2) 在浏览器里打开官方单曲页并点「播放」。
    # 网页版点播放会把歌加进播放列表，但腾讯对未登录/VIP 歌曲会弹登录框，
    # 所以这里只如实回报页面状态，绝不宣称「正在播放」。
    note = _ui_search_song(app, song)
    page = ("https://y.qq.com/n/ryqq_v2/songDetail/" + info["mid"]) if info and info.get("mid") \
        else "https://y.qq.com/n/ryqq_v2/search?w=" + urllib.parse.quote(song)
    web_note = ""
    try:
        bb = browser_cdp.get_browser()
        good, _ = bb.ensure()
        if good:
            bb.navigate(page)
            time.sleep(3.0)
            clicked = bb.click(text="播放")
            body = re.sub(r"\s+", " ", (bb.state().get("text") or ""))[:400]
            if "QQ登录" in body or "微信登录" in body or "下载客户端 体验更多内容" in body:
                web_note = ("浏览器已打开官方单曲页并点下「播放」，歌曲已进播放列表，"
                            "但网页版要求先登录 QQ/微信账号才会出声")
            elif clicked.get("clicked"):
                web_note = "浏览器已打开官方单曲页并点下「播放」"
            else:
                web_note = "浏览器已打开官方单曲页"
    except Exception as e:
        web_note = f"浏览器兜底未成功：{e}"
    return (f"找到了《{title}》{who}，但这台 Mac 暂时拿不到它的试听音频"
            f"（VIP / 版权 / 未登录）。{note} {web_note}。"
            f"请如实说明『已搜索并打开歌曲页，但要出声需要先登录账号』，不要断言正在播放。")


def _ui_search_song(app, song):
    """兜底：在音乐客户端真实搜索框里输入歌名并回车（结果页自绘，读不到列表）。"""
    try:
        pid, _ = find_app_pid(app["bundle_id"], app["name"])
        if not pid or not accessibility_ok():
            return f"{app['name']} 未聚焦或辅助功能未授权，无法自动搜索。"
        activate_pid(pid)
        _, tree = app_ui(pid, depth=7)
        box = None
        for n in tree.nodes():
            s = n.get("size")
            if (isinstance(s, list) and 100 < s[0] < 420 and 15 < s[1] < 60
                    and "搜索" in str(n.get("desc") or "") + str(n.get("title") or "")):
                box = n
                break
        if not box:
            return f"没在 {app['name']} 界面里找到搜索框。"
        p, s = box["pos"], box["size"]
        click_mac(p[0] + s[0] / 2, p[1] + s[1] / 2)
        time.sleep(0.3)
        foc = ax_ui.app_element(pid).attr("AXFocusedUIElement")
        if isinstance(foc, ax_ui.AXElement):
            foc.set("AXValue", "")
        paste_text(song)
        time.sleep(0.4)
        hotkey(36)
        return f"已在 {app['name']} 的搜索框输入「{song}」并回车搜索。"
    except Exception as e:
        return f"自动搜索异常：{e}"


# ---------------------------------------------------------------------------
# 浏览器控制：真实打开 Chrome → 在搜索框输入中文 → 按回车 → 读回网页文字 → 点结果。
# 之前做不到，是因为 macOS 辅助功能只能读到 Chrome 的窗口骨架，网页 DOM 读不到；
# 现在用 CDP（browser_cdp.py，纯标准库）拿到真实页面内容。
# ---------------------------------------------------------------------------
SEARCH_HOME = "https://cn.bing.com/"
_CN_DIGIT = {"一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5,
             "六": 6, "七": 7, "八": 8, "九": 9, "十": 10}


def parse_browser_request(text):
    """把中文口语整句解析成浏览器动作。句式只在这里维护，固件不写死任何话术。"""
    s = (text or "").strip()
    d = {"action": "", "query": "", "url": "", "target": "", "index": 0, "key": "Enter"}
    if not s:
        return d

    m = re.search(r"(https?://[^\s，。、]+)", s)
    if m:
        d.update(action="open", url=m.group(1))
        return d

    # 「点第 2 个结果 / 打开第三条链接」
    m = re.search(r"(?:点|打开|选)(?:击|一下|下)?\s*第?\s*([一二三四五六七八九十两\d]+)\s*"
                  r"(?:个|条)\s*(?:结果|链接|搜索结果)?", s)
    if m and ("结果" in s or "链接" in s or "条" in s):
        raw = m.group(1)
        d.update(action="click", index=_CN_DIGIT.get(raw, int(raw) if raw.isdigit() else 1) - 1)
        return d

    # 「点播放 / 点击试听 / 点登录」
    m = re.search(r"(?:点击|点一下|点|按一下|按)\s*[「『\"']?([^\s，。、」』\"']{1,10})", s)
    if m and any(k in s for k in ("播放", "试听", "登录", "下一页", "更多", "确定", "搜索", "视频")):
        d.update(action="click", target=m.group(1).strip("「」『』\"'"))
        return d

    if any(k in s for k in ("按回车", "敲回车", "按一下回车", "回车", "按确定", "确认")):
        d.update(action="enter", key="Enter")
        return d
    if any(k in s for k in ("读一下", "读读", "念一下", "网页上", "页面上", "屏幕上写",
                            "写了什么", "看看结果", "搜索结果是什么", "有什么内容", "显示了什么")):
        d.update(action="read")
        return d
    if any(k in s for k in ("返回上一页", "上一页", "后退", "返回")):
        d.update(action="back")
        return d
    if "输入" in s:
        m = re.search(r"输入\s*[「『\"':：]?\s*([^\s，。、」』\"']{1,80})", s)
        if m:
            d.update(action="type", query=m.group(1))
            return d

    # 搜索：取「搜索/搜/查/找」之后的原文，尽量不破坏查询内容
    m = re.search(r"(搜索一下|搜一下|搜索|搜搜|搜|查一下|查找|查|找一下|找找|找|百度一下)", s)
    if m:
        q = s[m.end():]
    else:
        q = re.sub(r"^(?:帮我|请|麻烦|先|再|然后|接着|给我)?\s*"
                   r"(?:打开|启动|开一下)\s*(?:一下)?\s*"
                   r"(?:浏览器|网页|Chrome|谷歌|谷歌浏览器|百度)\s*"
                   r"(?:然后|再|并|接着)?\s*[,，、]?\s*", "", s)
    q = re.sub(r"(来播放|播放|按回车|敲回车|回车|一下|就行了|好不好|可以吗)$", "", q.strip())
    q = q.strip(" ，,。.、:：;；\"'「」『』的了啊呢吧吗")
    d.update(action="search" if q else "open", query=q)
    return d


def _focus_search_input(b):
    js = r"""(function(){
      var i = document.querySelector('input[name="q"]')
           || document.querySelector('input[type="search"]')
           || document.querySelector('input[type="text"]')
           || document.querySelector('input');
      if (!i) return 'no';
      i.focus(); i.value = '';
      i.dispatchEvent(new Event('input', {bubbles:true}));
      return 'ok';
    })()"""
    try:
        return b.eval(js)
    except Exception:
        return "no"


def _page_brief(st, limit=560):
    title = (st.get("title") or "").strip()
    url = (st.get("url") or "").strip()
    body = re.sub(r"\s+", " ", (st.get("text") or "")).strip()[:limit]
    links = [x["text"].strip() for x in (st.get("elements") or [])
             if x.get("tag") == "a" and (x.get("text") or "").strip()][:14]
    return title, url, body, links


def browser_action(d, raw_text=""):
    """执行浏览器动作，返回给小智的中文结果（含真实网页文字，可直接据此回答用户）。"""
    b = browser_cdp.get_browser()
    good, why = b.ensure()
    if not good:
        return {"status": "error", "error": why}

    action = (d.get("action") or "open").lower()
    query = (d.get("query") or "").strip()
    url = (d.get("url") or "").strip()
    target = (d.get("target") or "").strip()
    index = int(d.get("index") or 0)
    key = d.get("key") or "Enter"
    st, result = {}, ""

    def run():
        nonlocal st, result
        if action in ("enter", "key"):
            b.key(key)
            time.sleep(2.6)
            st = b.state()
            t, u, body, links = _page_brief(st)
            result = (f"已在浏览器里按下回车/确认。当前网页：{t or u}。"
                      f"网页上真实显示的内容：{body}")
        elif action == "read":
            st = b.state()
            t, u, body, links = _page_brief(st, 700)
            result = (f"浏览器当前网页：{t or u}（{u[:110]}）。"
                      f"网页上真实显示的内容：{body}。"
                      f"可点链接：{'、'.join(links[:10])}")
        elif action == "click":
            r = b.click(text=target, index=index)
            time.sleep(3.2)   # 留够本标签导航 + 页面加载的时间
            st = b.state()
            t, u, body, links = _page_brief(st)
            if not r.get("clicked"):
                result = (f"网页上没有找到「{target or '指定结果'}」这个可点元素。"
                          f"当前网页：{t}。可见链接：{'、'.join(links[:10])}")
            else:
                result = (f"已在浏览器里点击「{r.get('label') or target}」。"
                          f"当前网页：{t or u}。网页上真实显示的内容：{body}")
        elif action == "type":
            _focus_search_input(b)
            b.type_text(query or target)
            time.sleep(0.4)
            st = b.state()
            t, u, body, links = _page_brief(st, 220)
            result = f"已在浏览器里输入「{query or target}」，还没有按回车。当前网页：{t or u}。"
        elif action == "back":
            b.eval("history.back()")
            time.sleep(2.2)
            st = b.state()
            t, u, body, links = _page_brief(st)
            result = f"已返回上一页。当前网页：{t or u}。网页内容：{body}"
        elif action == "search":
            if not query:
                result = "没听清要搜索什么，请说出要搜的内容。"
                return
            b.navigate(SEARCH_HOME)
            time.sleep(2.5)
            if _focus_search_input(b) == "no":
                b.navigate("https://cn.bing.com/search?q=" + urllib.parse.quote(query))
                time.sleep(3.0)
                how = "直接打开搜索结果页"
            else:
                b.type_text(query)
                b.key("Enter")
                time.sleep(3.4)
                how = "在搜索框输入并按下回车"
            st = b.state()
            t, u, body, links = _page_brief(st, 620)
            result = (f"已打开浏览器，{how}搜索「{query}」。当前网页：{t or u}。"
                      f"网页上真实显示的内容：{body}。"
                      f"主要链接：{'、'.join(links[:8])}。"
                      f"回答用户时只能依据上面这些真实网页内容，不要编造。")
        else:  # open
            dest = url or SEARCH_HOME
            b.navigate(dest)
            time.sleep(2.8)
            st = b.state()
            # 首次冷启动联网较慢，别把 about:blank 当成"浏览器已打开"就返回
            for _ in range(8):
                if (st.get("url") or "") not in ("", "about:blank"):
                    break
                time.sleep(0.6)
                st = b.state()
            t, u, body, links = _page_brief(st, 420)
            result = (f"已打开浏览器{'并访问 ' + dest if url else ''}。"
                      f"当前网页：{t or u}。网页上真实显示的内容：{body}。"
                      f"主要链接：{'、'.join(links[:8])}")

    try:
        run()
    except Exception as e:
        # Chrome 可能被系统回收导致 ws 失效：重置固定标签后重试一次
        try:
            b._active_ws = None
            b.ensure()
            run()
        except Exception as e2:
            return {"status": "error", "error": f"浏览器操作失败：{e}；重试：{e2}"}

    t, u, body, links = _page_brief(st)
    return {"status": "success", "result": result,
            "page": {"title": t, "url": u, "links": links[:12]}}


# ---------------------------------------------------------------------------
# 手机通道：与 Mac 平级的第二个设备后端。
#
# 数据契约对齐社区 xiaozhi-AutoGLM-mcp / Open-AutoGLM：
#   /phone_state -> {"a11y_tree": [...], "phone_state": {...}, "screen": [w,h]}
# 这样「现在屏幕显示什么、下一步点哪里」是真实回读，不是猜。
# 固件只把用户原话整句送到 /phone，中文句式、应用名、坐标全在这台电脑端解析。
# ---------------------------------------------------------------------------

def phone_devices():
    try:
        return android_adb.list_devices()
    except Exception:
        return []


def parse_phone_request(text):
    """把一句中文口语解析成手机上的原子动作序列。固件里没有任何句式/应用表。"""
    t = re.sub(r"\s+", "", (text or "").strip())
    d = {"action": "", "query": "", "text": "", "target": "", "index": 1,
         "direction": "", "confirm": "", "steps": []}
    if not t:
        d["action"] = "state"
        return d

    # 1) 敏感动作的二次确认：「确认执行 <token>」
    m = re.search(r"(?:确认执行|确认|确定)([0-9a-fA-F]{6,8})", t)
    if m:
        d["action"], d["confirm"] = "confirm_execute", m.group(1)
        return d

    # 2) 接入引导 / 应用列表 / 读屏 / 图片 OCR
    if any(w in t for w in ("接入引导", "引导安装", "连一下手机", "连接手机", "初始化手机")):
        d["action"] = "bootstrap"
        return d
    if any(w in t for w in ("有什么应用", "装了什么", "应用列表", "有哪些app", "有哪些APP", "能开什么")):
        d["action"] = "apps"
        return d
    if any(w in t for w in ("截屏识别", "图片识别", "OCR", "ocr", "屏幕识别")):
        d["action"] = "ocr"
        return d

    # 3) 应用名 / 搜索词（可出现在同一句里：「打开美团，找一下奶茶多少钱」）
    app = ""
    m = re.search(r"(?:打开|启动|进入|开一下)(?:手机上的|手机里的|用手机|在手机上|手机)?"
                  r"([^\s，,。！？、；:：]{1,24})", t)
    if m:
        app = re.sub(r"(然后|接着|再|帮我|给我|并|去|来).*$", "", m.group(1))
        # 复合句「打开抖音回复：xxx」/「打开微信给张三发消息说…」
        # 不要把后面的动词、联系人粘进应用名。
        app = re.sub(r"(回复|评论|发消息|发信息|发条消息|下单|结算|搜索|搜一下|"
                     r"查一下|找一下|找|看看|问一下|支付|付款|说|给).*$", "", app)
    q = ""
    m = re.search(r"(?:搜索|搜一下|搜|查找|查一下|查查|查|找一下|找|看看|问一下)"
                  r"(?:一个|一下|个)?([^，,。！？；:：]{1,30}?)"
                  r"(?:多少钱|的价格|价格|怎么样|贵不贵|来着|吧|呢)?"
                  r"(?:[，,。！？；]|$)", t)
    if m:
        q = re.sub(r"^(一个|一下|个)", "", m.group(1))
        q = re.sub(r"(多少钱|价格|怎么样)$", "", q)
        # 「搜索北京天气然后按回车」不要把后半句动作吞进搜索词
        q = re.sub(r"(然后|接着|之后|再|并且|顺手|顺便).*$", "", q).strip()

    # 4) 发送 / 回复 / 下单：一律走二次确认，不一句语音就直接执行
    sensitive = ""
    if any(w in t for w in ("下单", "提交订单", "结算", "支付", "付款")):
        sensitive = "order"
    elif any(w in t for w in ("发消息", "发条消息", "发信息", "回复", "评论", "发送")):
        sensitive = "message"
    if sensitive:
        m = re.search(r"(?:说|内容是|内容|：|:)([^\s，,。！？]{1,80})$", t)
        mc = re.search(r"给([^\s，,。！？:：]{1,20}?)(?:发消息|发信息|发条消息|发|说)", t)
        contact = mc.group(1).strip() if mc else ""
        d["action"] = "steps"
        if app:
            d["steps"].append({"action": "open", "query": app})
        if contact:
            # 先点开指定联系人，再输入内容，避免把消息发错窗口
            d["steps"].append({"action": "tap_text", "target": contact, "soft": True})
        if m:
            d["steps"].append({"action": "input", "text": m.group(1)})
        d["steps"].append({"action": sensitive,
                           "target": "提交订单" if sensitive == "order" else "发送"})
        return d

    if app or q:
        d["action"] = "steps"
        if app:
            d["steps"].append({"action": "open", "query": app})
        if q:
            # 先尝试点当前界面的搜索框；找不到不中断，仍可输入+回车
            d["steps"].append({"action": "tap_text", "target": "搜索", "soft": True})
            d["steps"].append({"action": "input", "text": q})
            d["steps"].append({"action": "enter"})
        return d

    # 5) 单步动作
    if any(w in t for w in ("按回车", "敲回车", "回车", "搜索键")):
        d["action"] = "enter"
        return d
    if any(w in t for w in ("回桌面", "主屏幕", "回到首页", "返回桌面")):
        d["action"] = "home"
        return d
    if any(w in t for w in ("返回", "上一页", "后退")):
        d["action"] = "back"
        return d
    for direction, words in (("up", ("上滑", "往上滑", "向上翻", "往下翻页")),
                             ("down", ("下滑", "往下滑", "向下翻")),
                             ("left", ("左滑", "往左滑")),
                             ("right", ("右滑", "往右滑"))):
        if any(w in t for w in words):
            d["action"], d["direction"] = "swipe", direction
            return d
    m = re.search(r"(?:点击|点|按)(?:一下)?(?:第([一二三四五六七八九十\d]+)个)?"
                  r"([^\s，,。！？]{1,20})", t)
    if m:
        d["action"] = "tap_text"
        d["target"] = m.group(2)
        idx = m.group(1) or "1"
        table = {"一": 1, "二": 2, "三": 3, "四": 4, "五": 5,
                 "六": 6, "七": 7, "八": 8, "九": 9, "十": 10}
        d["index"] = table.get(idx, int(idx) if idx.isdigit() else 1)
        return d
    m = re.search(r"(?:输入|打字|写入)(.+)", t)
    if m:
        d["action"], d["text"] = "input", m.group(1)
        return d
    d["action"] = "state"   # 没听出动作就读屏，让云端据此决定下一步
    return d


def phone_action(d, text=""):
    """顺序执行动作序列，每一步都回读真实屏幕；返回给小智的必须是设备上看得见的文字。"""
    steps = d.get("steps") or [d]
    out, screen, st = [], "", None
    for s in steps:
        action = s.get("action") or "state"
        args = {k: v for k, v in s.items() if k not in ("action", "soft") and v not in ("", None)}
        try:
            r, st = android_adb.act(action, **args)
        except android_adb.AdbError as e:
            if s.get("soft"):
                out.append(f"[{action} 跳过] {str(e)[:120]}")
                continue
            return err(str(e))
        except Exception as e:
            return err(f"手机操作异常：{type(e).__name__}: {e}")
        if st.get("need_confirm"):
            out.append(r)
            return {"status": "success", "result": "\n".join(out)[:2200],
                    "need_confirm": True, "token": st.get("token", "")}
        # 每步只留动作说明，完整屏幕文字最后统一附一次，避免重复占设备内存
        out.append(r.split("\n手机当前前台")[0][:260])

    try:
        st = android_adb.state()
        screen = android_adb.brief(st)
    except Exception as e:
        screen = f"（读屏失败：{e}）"
    # 纯读屏动作的结果本身已经带完整屏幕文字，不再重复贴一遍
    only_read = len(steps) == 1 and (steps[0].get("action") in ("state", "read", "screen"))
    result = ("\n".join(x for x in out if x) if only_read
              else "\n".join(x for x in out if x) + "\n【手机屏幕真实内容】\n" + screen)
    return {"status": "success", "result": result[:2200], "screen": screen,
            "a11y_tree": (st or {}).get("a11y_tree", []),
            "phone_state": (st or {}).get("phone_state", {})}


def pick_app_by_kw(words):
    """从这台设备真实安装的应用里挑一个，绝不写死包名/应用名。

    优先用桌面图标上读到的中文名（用户看得见的名字），其次才退到包名片段，
    这样「QQ音乐」不会被说成「qqmusic」。
    """
    try:
        for w in words:
            for n in android_adb.cached_launcher_names():
                if w.lower() in n.lower():
                    return n
    except Exception:
        pass
    try:
        apps = android_adb.installed_apps()
        for w in words:
            for a in apps:
                if w.lower() in (a["name"] + " " + a["package"]).lower():
                    return a["name"]
    except Exception:
        pass
    return ""


def phone_text_to_result(text, structured=None, dry=False):
    """手机自托管模式的整句入口：解析 → 去重 → 真实动作 → 回读屏幕。

    与 Mac 形态的 /phone 分支保持同一套语义（含敏感动作二次确认），
    这样固件 10 个工具无论指向 Mac 还是指向手机，行为都一致。
    """
    b = dict(structured or {})
    d = parse_phone_request(text or "")
    for f in ("action", "query", "target", "index", "direction", "confirm",
              "x", "y", "key", "content"):
        if b.get(f) not in (None, ""):
            d[f] = b[f]
    if d.get("content"):
        d["text"] = d.pop("content")
    if b.get("action"):
        d["steps"] = []
    if isinstance(b.get("steps"), list) and b["steps"]:
        d["steps"], d["action"] = b["steps"], "steps"
    if d.get("confirm") and d.get("action") != "confirm_execute" and not d.get("steps"):
        d["action"] = "confirm_execute"
    if dry:
        return ok("解析结果（未执行）：" + json.dumps(d, ensure_ascii=False))

    seq = d.get("steps") or [d]
    key = "|".join(f"{s.get('action')}:{s.get('query') or s.get('text') or s.get('target') or ''}"
                   for s in seq)
    now = time.time()
    readonly = d.get("action") in ("state", "read", "screen", "apps", "ocr", "bootstrap")
    if not readonly:
        with _lock:
            dup = (key == _last_phone["key"] and now - _last_phone["ts"] < 15)
            if not dup:
                _last_phone.update(key=key, ts=now, result="")
        if dup:
            tip = ("这个动作刚才已经执行过了，已自动跳过重复操作。"
                   f"上次结果：{_last_phone['result'][:180]}")
            return ok(tip)
    out = phone_action(d, text or "")
    _last_phone["result"] = out.get("result") or out.get("error") or ""
    log(f"  → 手机(自托管) {key} => {out.get('status')}: {_last_phone['result'][:200]}")
    record("phone", f"{key} => {_last_phone['result'][:200]}")
    if out.get("status") == "success":
        return ok(out["result"])
    return err(out.get("error", "手机操作失败"))


def ok(result="ok"):
    return {"status": "success", "result": result}


def err(msg):
    return {"status": "error", "error": msg}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _auth(self, path):
        if path in ("/ping", "/result"):
            return True
        if self.headers.get("Authorization", "") != f"Bearer {TOKEN}":
            self._send(err("unauthorized"), 401)
            return False
        return True

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if not raw:
            return {}
        try:
            return json.loads(raw)
        except Exception:
            return {}

    # -----------------------------------------------------------------------
    # 手机自托管形态：同一个固件、同一套 HTTP 路径，目标全部换成这台手机本身。
    # 这里刻意不复用任何 Mac 执行器，避免 Termux 里加载 Quartz/AppKit 崩溃。
    # -----------------------------------------------------------------------
    def _phone_get(self, path):
        A = android_adb
        try:
            if path == "/ping":
                return self._send(ok("pong"))
            if path == "/version":
                return self._send(ok("xiaozhi-phone-portal 2.0（手机本机运行："
                                    "真实控件树 / 动态应用 / 中文输入 / 二次确认；链路无电脑）"))
            if path == "/devices":
                phones = A.list_devices()
                ready = any(x["state"] == "device" for x in phones)
                payload = {"mac": {"online": False, "front_app": "", "app_count": 0},
                           "phones": phones, "phone_ready": ready,
                           "mode": "phone-only（ESP32 直连手机，不需要电脑）",
                           "port": self.server.server_address[1]}
                if not ready:
                    payload["phone_hint"] = A.setup_hint(phones)
                return self._send({"status": "success", "devices": payload})
            if path in ("/apps", "/phone_apps"):
                serial = A.require_device()["serial"]
                # Android 的 pm 只给包名；中文应用名只存在于桌面图标上。
                # 首次查询时回桌面读一次真实图标文字，之后走运行时缓存，
                # 固件和 Portal 里没有任何预置应用名单。
                names = A.cached_launcher_names()
                if not names:
                    try:
                        names = A.launcher_names(serial, go_home=True)
                    except Exception:
                        names = []
                apps = A.installed_apps(serial)
                learned = [a["name"] for a in apps if a["aliases"]]
                merged = list(dict.fromkeys(list(names) + learned))[:80]
                if "brief=1" in self.path or path == "/apps":
                    return self._send(ok("、".join(merged or [a["name"] for a in apps[:80]])))
                return self._send(ok(json.dumps(
                    [{"name": a["name"], "bundle_id": a["package"],
                      "aliases": a["aliases"]} for a in apps], ensure_ascii=False)))
            if path in ("/state", "/state_full", "/ui", "/phone_state"):
                st = A.state()
                if path == "/phone_state":
                    return self._send({"status": "success", "state": st})
                return self._send(ok(json.dumps({
                    "a11y_tree": st.get("a11y_tree", []),
                    "phone_state": st.get("phone_state", {}),
                    "_ui_lines": st.get("lines", []),
                    "_accessibility_trusted": True,
                    "_device": "android-self-hosted",
                }, ensure_ascii=False)))
            if path in ("/screen", "/browser_state", "/music_state"):
                return self._send(ok(A.brief(A.state())))
            if path == "/phone_bootstrap":
                return self._send(ok(A.bootstrap()))
            if path == "/result":
                st = A.state()
                return self._send(ok(json.dumps({
                    "device": "这台手机（自托管）",
                    "front_app": st.get("phone_state", {}).get("currentApp", ""),
                    "actions": _actions[-8:],
                    "screen": A.brief(st, 400),
                }, ensure_ascii=False)))
            return self._send(err(f"手机模式不提供 {path}"), 404)
        except A.AdbError as e:
            return self._send(err(str(e)))
        except Exception as e:
            return self._send(err(f"手机读屏异常：{type(e).__name__}: {e}"))

    def _phone_post(self, path, b):
        A = android_adb
        try:
            if path == "/phone":
                return self._send(phone_text_to_result(
                    (b.get("text") or "").strip(), b, bool(b.get("dry_run"))))
            if path in ("/send_message", "/play_music", "/browser"):
                text = (b.get("text") or "").strip()
                if path == "/send_message" and not text:
                    c, m = (b.get("contact") or "").strip(), (b.get("message") or "").strip()
                    a = (b.get("app") or "").strip() or pick_app_by_kw(("微信", "QQ", "短信", "信息"))
                    text = (f"打开{a}给{c}发消息说{m}" if c and a
                            else f"给{c}发消息说{m}" if c else f"发消息说{m}")
                if path == "/play_music" and not text:
                    song = (b.get("song") or "").strip()
                    app = ((b.get("app") or "").strip()
                           or pick_app_by_kw(("音乐", "QQ", "酷狗", "网易", "汽水")))
                    steps = ([{"action": "open", "query": app}] if app else [])
                    steps += [{"action": "tap_text", "target": "搜索", "soft": True},
                              {"action": "input", "text": song},
                              {"action": "enter"}]
                    return self._send(phone_text_to_result("", {"steps": steps}))
                if path == "/browser" and not text:
                    if (b.get("action") or "") == "read":
                        return self._send(ok(A.brief(A.state())))
                    q = (b.get("query") or "").strip()
                    app = ((b.get("app") or "").strip()
                           or pick_app_by_kw(("浏览器", "Chrome", "夸克", "百度", "Safari")))
                    steps = ([{"action": "open", "query": app}] if app else [])
                    steps += [{"action": "tap_text", "target": "搜索", "soft": True},
                              {"action": "input", "text": q}, {"action": "enter"}]
                    return self._send(phone_text_to_result("", {"steps": steps}))
                if path == "/browser" and text:
                    # 「打开浏览器」是泛称：换成这台手机真实装着的浏览器中文名，
                    # 找不到时保留原样，让错误里带回真实应用列表，不编造应用。
                    d = parse_phone_request(text)
                    steps = d.get("steps") or []
                    for s in steps:
                        q0 = s.get("query") or ""
                        if s.get("action") == "open" and any(
                                w in q0 for w in ("浏览器", "browser", "上网")):
                            real = pick_app_by_kw(("浏览器", "Chrome", "chrome", "夸克",
                                                  "百度", "Safari", "QQ浏览器", "UC", "Edge"))
                            if real:
                                s["query"] = real
                    return self._send(phone_text_to_result(
                        "", {"steps": steps} if steps else d))
                return self._send(phone_text_to_result(text, {}))
            if path in ("/app", "/open_app"):
                pkg = (b.get("package") or b.get("app") or "").strip()
                key, now = pkg, time.time()
                with _lock:
                    dup = (key == _last_phone_app["key"] and now - _last_phone_app["ts"] < 15)
                    if not dup:
                        _last_phone_app.update(key=key, ts=now)
                if dup:
                    return self._send(ok(f"「{pkg}」刚才已经打开过了，已跳过重复打开。"))
                r = A.open_app(pkg)
                msg = (f"已在这台手机上打开「{r['app']['name']}」({r['app']['package']})。"
                       f"{r['hint']}\n" + A.brief(A.state()))
                log("  → " + msg[:200])
                record("phone_app", msg[:240])
                return self._send(ok(msg[:2200]))
            if path in ("/tap", "/click"):
                x, y = int(b.get("x", 0) or 0), int(b.get("y", 0) or 0)
                # 固件把 0-1000 相对坐标放大成 1080x2400，这里还原，
                # 再由 android_adb 按真机 wm size 映射到实际像素。
                if max(x, y) > 1000:
                    x, y = int(x * 1000 / 1080), int(y * 1000 / 2400)
                r, _ = A.act("tap", x=x, y=y)
                record("tap", f"({x},{y})")
                return self._send(ok(r[:900]))
            if path == "/swipe":
                r, _ = A.act("swipe", startX=b.get("startX", 0), startY=b.get("startY", 0),
                             endX=b.get("endX", 0), endY=b.get("endY", 0),
                             direction=b.get("direction") or "up")
                return self._send(ok(r[:900]))
            if path in ("/keyboard/input", "/input_text", "/input"):
                try:
                    text = base64.b64decode(b.get("base64_text", "")).decode("utf-8")
                except Exception:
                    text = ""
                text = text or (b.get("text") or "")
                r, _ = A.act("input", text=text)
                _typed_text.append(text)
                return self._send(ok(r[:900]))
            if path in ("/keyboard/key", "/key"):
                code = int(b.get("key_code") or b.get("key") or 66)
                mapping = {3: "home", 4: "back", 66: "enter", 67: "key"}
                r, _ = A.act(mapping.get(code, "key"), key=code)
                return self._send(ok(r[:900]))
            if path in ("/screen", "/state"):
                return self._send(ok(A.brief(A.state())))
            if path == "/ping":
                return self._send(ok("pong"))
            return self._send(err(f"手机模式不提供 {path}"), 404)
        except A.AdbError as e:
            return self._send(err(str(e)))
        except Exception as e:
            return self._send(err(f"手机操作异常：{type(e).__name__}: {e}"))

    def do_GET(self):
        path = self.path.split("?")[0]
        log(f"GET {path}")
        if not self._auth(path):
            return
        if PHONE_ONLY:
            return self._phone_get(path)
        if path == "/ping":
            return self._send(ok("pong"))
        if path == "/version":
            return self._send(ok("computer-portal 1.5 (统一设备控制面：Mac 动态应用/控件树/发消息/点歌/浏览器 + Android 手机 ADB 真实读屏/点击/中文输入/动态应用发现)"))
        if path == "/devices":
            # 让小智知道「现在到底接入了什么设备」，而不是固件里写死一台电脑。
            phones = phone_devices()
            try:
                mac_apps = installed_apps()
                mac_count, mac_front = len(mac_apps), str(front_app())
            except Exception:
                mac_count, mac_front = 0, ""
            payload = {"mac": {"online": True, "front_app": mac_front, "app_count": mac_count},
                       "phones": phones,
                       "phone_ready": any(x["state"] == "device" for x in phones)}
            if not payload["phone_ready"]:
                # 没接手机时把「怎么接」如实告诉小智，让它转述而不是回答做不到
                payload["phone_hint"] = android_adb.setup_hint(phones)
            return self._send({"status": "success", "devices": payload})
        if path == "/phone_state":
            try:
                return self._send({"status": "success", "state": android_adb.state()})
            except android_adb.AdbError as e:
                return self._send(err(str(e)))
        if path == "/phone_apps":
            try:
                serial = android_adb.require_device()["serial"]
                apps = android_adb.installed_apps(serial)
                st = android_adb.state(serial)
                visible = "、".join(st["lines"][:30])
                desk = android_adb.cached_launcher_names()
                if "brief=1" in self.path:
                    return self._send(ok("手机真实安装：" + "、".join(a["name"] for a in apps[:60])
                                        + ("；桌面应用：" + "、".join(desk[:30]) if desk else "")
                                        + ("；当前屏幕文字：" + visible if visible else "")))
                return self._send(ok(json.dumps(
                    {"apps": [{"name": a["name"], "package": a["package"],
                               "activity": a["activity"], "aliases": a["aliases"]} for a in apps],
                     "screen_visible": visible}, ensure_ascii=False)))
            except android_adb.AdbError as e:
                return self._send(err(str(e)))
        if path == "/phone_bootstrap":
            try:
                return self._send(ok(android_adb.bootstrap()))
            except android_adb.AdbError as e:
                return self._send(err(str(e)))
        if path == "/browser_state":
            # 只读：把浏览器当前真实页面文字给设备端/调试用
            out = browser_action({"action": "read"})
            if out.get("status") == "success":
                return self._send(ok(out["result"]))
            return self._send(err(out.get("error", "读不到浏览器页面")))
        if path == "/music_state":
            apps = installed_apps()
            app = None
            for kw in ("QQ音乐", "QQMusic", "网易云音乐", "Music"):
                cand, _ = resolve_app(kw)
                if cand:
                    app = cand
                    break
            if not app:
                return self._send(err("没有找到音乐应用"))
            cur, playing = music_now_playing(app["bundle_id"], app["name"])
            return self._send(ok(f"{app['name']} 当前歌曲：{cur or '未读到'}；正在播放={playing}"))
        if path == "/screen":
            # 给 ESP32 的精简版：纯文本，控制长度，避免吃掉设备内存
            return self._send(ok(screen_summary()))
        if path == "/ui":
            # 真实屏幕状态：前台应用 + 真实控件树（文字、按钮、输入框、焦点、坐标）
            info, tree = app_ui(depth=6)
            return self._send(ok(json.dumps({
                "app": info,
                "trusted": accessibility_ok(),
                "lines": ui_texts(tree, 160),
                "tree": tree.to_json(),
            }, ensure_ascii=False)))
        if path == "/parse_message":
            # 调试用：看看一句中文口话被解析成什么，不产生任何动作
            raw = urllib.parse.unquote(self.path.split("text=", 1)[1]) if "text=" in self.path else ""
            return self._send(ok(json.dumps(parse_message_request(raw), ensure_ascii=False)))
        if path == "/apps":
            # 真实安装列表：设备端 / 大模型通过它知道这台电脑上到底有什么 App。
            apps = installed_apps(force="refresh=1" in self.path)
            if "brief=1" in self.path:
                return self._send(ok("、".join(app_names(apps))))
            return self._send(ok(json.dumps(
                [{"name": a["name"], "bundle_id": a["bundle_id"], "aliases": a["aliases"]}
                 for a in apps], ensure_ascii=False)))
        if path == "/result":
            with _lock:
                p = mouse_pos()
                return self._send(ok(json.dumps({
                    "actions": _actions,
                    "typed": _typed_text,
                    "clipboard": clipboard(),
                    "mouse": [round(p.x), round(p.y)],
                    "front_app": front_app(),
                }, ensure_ascii=False)))
        if path in ("/state", "/state_full"):
            # 用真实控件树替换早期那份写死的假树：小智/AutoGLM 现在能真正看到屏幕上有什么
            info, tree = app_ui(depth=6)
            sw, sh = screen_size()

            def norm(n):
                if not n:
                    return None
                p, s = n.get("pos"), n.get("size")
                bounds = ""
                if isinstance(p, list) and isinstance(s, list) and sw and sh:
                    x1, y1 = int(p[0] * 1080 / sw), int(p[1] * 2400 / sh)
                    x2, y2 = x1 + int(s[0] * 1080 / sw), y1 + int(s[1] * 2400 / sh)
                    bounds = f"{x1},{y1},{x2},{y2}"
                text = str(n.get("title") or n.get("value") or "")
                return {
                    "text": text[:120], "contentDescription": str(n.get("desc") or ""),
                    "className": n.get("role") or "", "bounds": bounds,
                    "resourceId": str(n.get("identifier") or ""),
                    "focused": bool(n.get("focused")),
                    "children": [norm(c) for c in n.get("children", [])][:40],
                }

            payload = {"a11y_tree": [norm(tree.root)] if tree.root else [],
                       "phone_state": {"currentApp": info.get("name", ""),
                                       "packageName": info.get("bundle_id", "com.apple.mac"),
                                       "activityName": "MacDesktop", "keyboardVisible": False},
                       "_mac_screen": [int(sw), int(sh)],
                       "_ui_lines": ui_texts(tree, 100),
                       "_accessibility_trusted": accessibility_ok()}
            return self._send(ok(json.dumps(payload, ensure_ascii=False)))
        return self._send(err(f"unknown path {path}"), 404)

    def do_POST(self):
        path = self.path.split("?")[0]
        if not self._auth(path):
            return
        b = self._body()
        if PHONE_ONLY:
            return self._phone_post(path, b)
        if path == "/tap":
            x, y = int(b.get("x", 0)), int(b.get("y", 0))
            want, real = move_and_click(x, y)
            detail = f"点击手机坐标({x},{y}) → Mac({want[0]},{want[1]})，实际鼠标位置({real[0]},{real[1]})"
            log("  → " + detail)
            record("tap", detail)
            return self._send(ok())
        if path == "/swipe":
            x1, y1 = int(b.get("startX", 0)), int(b.get("startY", 0))
            x2, y2 = int(b.get("endX", 0)), int(b.get("endY", 0))
            want, real = move_and_click(x2, y2)
            detail = f"滑动 ({x1},{y1})→({x2},{y2})，鼠标实际到({real[0]},{real[1]})"
            log("  → " + detail)
            record("swipe", detail)
            return self._send(ok())
        if path == "/keyboard/input":
            try:
                text = base64.b64decode(b.get("base64_text", "")).decode("utf-8")
            except Exception:
                text = ""
            type_unicode(text)
            detail = f"注入文本 {text!r} (clear={b.get('clear')})"
            log("  → " + detail)
            record("input", detail)
            return self._send(ok())
        if path == "/keyboard/key":
            code = b.get("key_code", 0)
            success = press_key(code)
            detail = f"按键 keycode={code} → {'已注入' if success else '未映射'}"
            log("  → " + detail)
            record("key", detail)
            return self._send(ok())
        if path == "/app":
            pkg = b.get("package", "")
            app, apps = resolve_app(pkg)
            if app is None:
                # 自动路由：这台 Mac 没装、但接了手机，就直接从手机桌面动态找到并打开。
                # 用户不必说「手机上的美团」，也不用在固件/数据库里预置任何 App 名单。
                phones = phone_devices()
                if any(x["state"] == "device" for x in phones):
                    key, now = pkg, time.time()
                    with _lock:
                        dup = (key == _last_phone_app["key"] and now - _last_phone_app["ts"] < 15)
                        if not dup:
                            _last_phone_app.update(key=key, ts=now)
                    if dup:
                        return self._send(ok(f"「{pkg}」刚才已经在手机上打开过了，已跳过重复打开。"))
                    try:
                        r = android_adb.open_app(pkg)
                        st = android_adb.state()
                        msg = (f"这台 Mac 上没有「{pkg}」，已在接入的手机上打开"
                               f"「{r['app']['name']}」({r['app']['package']})。{r['hint']}\n"
                               + android_adb.brief(st))
                        log("  → " + msg[:200])
                        record("phone_app", msg[:240])
                        return self._send(ok(msg[:2200]))
                    except android_adb.AdbError as e:
                        return self._send(err(f"这台 Mac 和接入的手机上都没找到「{pkg}」。{e}"))
                msg = (f"这台 Mac 上没有找到应用「{pkg}」。实际已安装："
                       + "、".join(app_names(apps, 40)))
                log("  → " + msg)
                record("app", msg)
                # 用 200 + {"status":"error"} 而不是 404：设备端 HTTP 客户端在非 200 时
                # 只保留状态码，会丢掉这份「真实可用应用列表」，模型就无法自我纠正。
                return self._send(err(msg))
            try:
                okflag = subprocess.run(["open", "-a", app["path"]],
                                        timeout=8, capture_output=True).returncode == 0
            except Exception:
                okflag = False
            detail = (f"启动 {pkg!r} → 动态解析为 {app['name']} ({app['path']}) → "
                      f"{'open -a 已执行' if okflag else '执行失败'}")
            log("  → " + detail)
            record("app", detail)
            if okflag:
                pid2, _ = find_app_pid(app["bundle_id"], app["name"])
                if pid2:
                    note_target(pid2, app["name"])
                return self._send(ok(f"已在这台 Mac 电脑上打开应用「{app['name']}」。"))
            return self._send(err(detail))
        if path == "/play_music":
            # 支持结构化 song/app，也支持把用户整句原文交给 text 由电脑端解析。
            song = (b.get("song") or "").strip()
            app_q = (b.get("app") or "").strip()
            text = (b.get("text") or "").strip()
            if text and not song:
                song, guessed = parse_music_request(text)
                if guessed and not app_q:
                    app_q = guessed
            if not song:
                return self._send(err("没听清歌名：song / text 都为空"))
            # 同一句话会被「设备端本地兜底」和「云端 MCP 工具」各执行一次，
            # 不去重会把同一首歌连播两遍（后一次打断前一次）。
            dry = bool(b.get("dry_run"))
            # key 只按歌名，不带应用：本地兜底走整句 text 会解析出「QQ音乐」，
            # 云端 tools/call 只填 song，两者若把 app 算进 key 就会各播一次打断自己。
            key, now = song.strip("《》 "), time.time()
            if not dry:
                with _lock:
                    dup = (key == _last_music["key"] and now - _last_music["ts"] < 15)
                    if not dup:
                        _last_music.update(key=key, ts=now, result="")
                if dup:
                    tip = f"这首歌刚才已经点过了，为避免重复播放已跳过。上次结果：{_last_music['result'][:120]}"
                    log("  → " + tip)
                    return self._send(ok(tip))
            result = play_music(song, app_q, dry_run=dry)
            if not dry:
                _last_music["result"] = result
            detail = f"播放 {song!r} app={app_q!r} → {result}"
            log("  → " + detail)
            record("music", detail)
            return self._send(ok(result))
        if path == "/phone":
            # 统一手机入口：固件只转发用户原话，中文句式/应用名/坐标都在这里解析。
            # 结构化字段（action/query/...）优先，便于云端模型做 observe→act 多轮循环。
            text = (b.get("text") or "").strip()
            d = parse_phone_request(text)
            for f in ("action", "query", "target", "index", "direction", "confirm",
                      "x", "y", "key", "content"):
                if b.get(f) not in (None, ""):
                    d[f] = b[f]
            if d.get("content"):
                d["text"] = d.pop("content")   # 结构化输入内容，跟整句原话 text 分开
            if b.get("action"):
                d["steps"] = []          # 给了结构化动作就不要再跑整句解析出的序列
            if isinstance(b.get("steps"), list) and b["steps"]:
                d["steps"] = b["steps"]
                d["action"] = "steps"
            # 云端模型也可以把确认码放在 confirm 字段而不是复述原话
            if d.get("confirm") and d.get("action") != "confirm_execute" and not d.get("steps"):
                d["action"] = "confirm_execute"
            if b.get("dry_run"):
                return self._send(ok("解析结果（未执行）：" + json.dumps(d, ensure_ascii=False)))

            # 本地 STT 兜底与云端 tools/call 会各来一次；按「解析后的动作序列」去重，
            # 不能把用户原话算进 key，否则两个通道的原文略有差异就会重复点击/发送。
            seq = d.get("steps") or [d]
            key = "|".join(f"{s.get('action')}:{s.get('query') or s.get('text') or s.get('target') or ''}"
                           for s in seq)
            now = time.time()
            readonly = d.get("action") in ("state", "read", "screen", "apps", "ocr", "bootstrap")
            if not readonly:
                with _lock:
                    dup = (key == _last_phone["key"] and now - _last_phone["ts"] < 15)
                    if not dup:
                        _last_phone.update(key=key, ts=now, result="")
                if dup:
                    tip = (f"这个手机动作刚才已经执行过了，已自动跳过重复操作。"
                           f"上次结果：{_last_phone['result'][:180]}")
                    log("  → " + tip)
                    return self._send(ok(tip))
            out = phone_action(d, text)
            _last_phone["result"] = out.get("result") or out.get("error") or ""
            log(f"  → 手机 {key} => {out.get('status')}: {_last_phone['result'][:240]}")
            record("phone", f"{key} => {_last_phone['result'][:240]}")
            if out.get("status") == "success":
                return self._send(ok(out["result"]))
            return self._send(err(out.get("error", "手机操作失败")))
        if path == "/browser":
            # 复合指令：打开浏览器 → 搜索 → 按回车 → 读网页 → 点结果。
            # 固件只转发用户原话，句式解析、应用/网址都不写死在固件里。
            text = (b.get("text") or "").strip()
            d = parse_browser_request(text)
            for f in ("action", "query", "url", "target", "index", "key"):
                if b.get(f) not in (None, ""):
                    d[f] = b[f]
            if not d.get("action"):
                d["action"] = "open"
            if b.get("dry_run"):
                return self._send(ok("解析结果（未执行）：" + json.dumps(d, ensure_ascii=False)))

            # 同一句语音会同时触发本地兜底和云端 MCP 工具，浏览器动作也必须去重，
            # 否则会连开两个标签页、连按两次回车。只读动作不占去重。
            bkey = "|".join(str(d.get(x, "")) for x in
                            ("action", "query", "url", "target", "index", "key"))
            now = time.time()
            if d["action"] != "read":
                with _lock:
                    dup = (bkey == _last_browser["key"] and now - _last_browser["ts"] < 15)
                    if not dup:
                        _last_browser.update(key=bkey, ts=now, result="")
                if dup:
                    tip = (f"这个浏览器动作刚才已经执行过了，已自动跳过重复操作。"
                           f"上次结果：{_last_browser['result'][:180]}")
                    log("  → " + tip)
                    return self._send(ok(tip))
            out = browser_action(d, text)
            _last_browser["result"] = out.get("result") or out.get("error") or ""
            detail = f"浏览器 {d} → {out.get('status')}: {_last_browser['result'][:240]}"
            log("  → " + detail)
            record("browser", detail)
            if out.get("status") == "success":
                return self._send(ok(out["result"]))
            return self._send(err(out.get("error", "浏览器操作失败")))
        if path == "/send_message":
            # 复合指令：打开微信/QQ → 找指定联系人 → 发指定内容。
            # 结构化字段优先；只给 text 时由电脑端解析中文口语句。
            #
            # 去重很关键：同一句话会同时触发「设备端本地 STT 兜底」和「云端 MCP 工具调用」，
            # 两条路径最终都到这里。统一按解析后的 (应用,联系人,内容) 去重，避免把消息发两遍。
            parsed = parse_message_request(b.get("text") or "")
            c = (b.get("contact") or parsed.get("contact") or "").strip()
            m = (b.get("message") or parsed.get("message") or "").strip()
            a = (b.get("app") or parsed.get("app") or "微信").strip()
            key, now = f"{a}|{c}|{m}", time.time()
            with _lock:
                dup = (key == _last_msg["key"] and now - _last_msg["ts"] < 15)
                if not dup:
                    _last_msg.update(key=key, ts=now, result="")
            if dup:
                tip = f"这条消息刚才已经执行过了，为避免重复发送已自动跳过。上次结果：{_last_msg['result'][:110]}"
                log("  → " + tip)
                return self._send(ok(tip))
            out = send_message(app_query=(b.get("app") or "").strip(),
                               contact=(b.get("contact") or "").strip(),
                               message=(b.get("message") or "").strip(),
                               raw_text=(b.get("text") or "").strip(),
                               dry_run=bool(b.get("dry_run")))
            _last_msg["result"] = out.get("result") or out.get("error") or "已执行"
            # 未登录：挂起这条消息，等用户在手机上确认登录后自动补发，不用重复口令。
            if (out.get("status") == "need_login" and not b.get("dry_run")
                    and c and m):
                target, _ = resolve_app(a)
                if target:
                    token = _pending["token"] + 1
                    payload = dict(app_query=a, contact=c, message=m,
                                   raw_text="", dry_run=False)
                    _pending.update(token=token, payload=payload)
                    threading.Thread(target=watch_login_and_send,
                                     args=(target["bundle_id"], target["name"],
                                           payload, token),
                                     daemon=True).start()
                    log(f"  → 已挂起待发送：{a}/{c}/{m}，检测到登录后自动补发")
            detail = f"发消息 app={b.get('app')!r} contact={out.get('contact', b.get('contact'))!r} " \
                     f"message={out.get('message', b.get('message'))!r} → {out.get('status')}: " \
                     f"{out.get('result') or out.get('error')}"
            log("  → " + detail)
            record("send_message", detail)
            if out.get("status") == "success":
                return self._send(ok(out.get("result", "ok")))
            return self._send(err(out.get("error", "发送失败")))
        return self._send(err(f"unknown path {path}"), 404)


if __name__ == "__main__":
    print(f"Computer Portal 监听 0.0.0.0:{PORT}，令牌={TOKEN!r}", flush=True)
    print("这个服务会真的移动鼠标 / 点击 / 打开应用 / 注入文本", flush=True)
    print("动作记录: ~/computer_portal.log，也可 GET /result 读取", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
