#!/usr/bin/env python3
"""
android_adb.py —— 手机侧执行器（Portal 的第二个设备后端）

跟 Mac 后端（ax_ui / browser_cdp）平级。设计原则：

1. 固件里没有也不需要任何 App 名单、包名、句式、坐标。
   /phone 只把用户原话整句送进来，电脑端从**这台真实接入的手机**上动态读取
   已安装应用（pm list packages）、当前界面（uiautomator dump）、当前前台包名
   （dumpsys window），再决定下一步动作。
2. 参考社区 xiaozhi-AutoGLM-mcp / Open-AutoGLM 的数据契约：返回
   {"a11y_tree": [...], "phone_state": {...}}，控件带 text/content-desc/bounds，
   所以「知道屏幕现在显示什么、下一步点哪里」是真实回读，不是猜。
3. 手机截屏走 `adb exec-out screencap -p`，不需要任何系统权限；
   自绘界面（Flutter/游戏/短视频）uiautomator 读不到时，用 macOS Vision
   本地 OCR 兜底。这跟「用 ESP32 摄像头拍屏幕」完全不同，是设备原生像素。
4. 安全：发送/下单/支付/删除等敏感动作一律二次确认，且只操作本人已授权设备。
   不提供任何绕过验证码、风控或平台检测的能力。

通道：手机 USB 接这台 Mac 走 adb，或同一局域网走无线 adb（tcpip 5555）。
ESP32 不需要物理接手机，只负责语音上云；链路见 PHONE_CONTROL.md。

自测：XIAOZHI_ADB 环境变量可指向 mock_adb.py，模拟一台安卓机跑通全链路。
"""
import base64
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
LABELS_FILE = Path.home() / ".xiaozhi_phone_labels.json"
KEYBOARD_APK = HERE / "ADBKeyboard.apk"
ADB_IME = "com.android.adbkeyboard/.AdbIME"

# 需要二次确认的动作：涉及账号、资金、对外内容，绝不一句语音就直接执行
SENSITIVE = ("message", "send", "order", "pay", "delete", "reply", "post", "confirm_execute")
# 明确不自动执行的动作：涉及支付/资金，只把界面读回来让用户本人确认
FORBID_AUTO = ("pay", "payment", "password", "captcha")

_confirm = {"token": "", "action": "", "payload": None, "ts": 0}


class AdbError(Exception):
    """带中文操作指引的错误，交给小智原样转述，不伪装成「做不到」。"""


def adb_bin():
    env = os.environ.get("XIAOZHI_ADB")
    if env:
        return env
    p = HERE / "platform-tools" / "adb"
    if p.exists():
        return str(p)
    return shutil.which("adb") or "adb"


def run(args, timeout=15, binary=False):
    cmd = [adb_bin()] + [str(a) for a in args]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout)
    except FileNotFoundError:
        raise AdbError("找不到 adb。请把 Android platform-tools 放到 tools/platform-tools/，"
                       "或设置 XIAOZHI_ADB 指向 adb 可执行文件。")
    except subprocess.TimeoutExpired:
        raise AdbError(f"adb 命令超时（{timeout}s）：{' '.join(cmd[:5])}")
    if binary:
        return r.stdout, r.stderr
    return r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def self_hosted():
    """Portal 是否跑在手机自己身上（Termux）——此时没有电脑可依赖。"""
    return os.environ.get("XIAOZHI_PHONE_ONLY", "").strip() == "1"


def termux_hint():
    """无电脑方案：本机「无线调试」自连（社区 LADB / Termux-MCP 的成熟做法）。

    Android 12+ 限制普通 App 进程直接跑 input/uiautomator/pm，所以必须让手机
    **自己连自己** 的 adb；这一步由系统自带的无线调试提供，不需要 root，
    也不需要另一台电脑。配对只需一次，之后端口变化由 mDNS 自动恢复。
    """
    return ("当前是「手机自托管」模式，服务就跑在这台手机上，不需要电脑。\n"
            "还没连上本机无线调试，一次性设置（只需做一次）：\n"
            "1) 手机：设置 → 关于手机 → 连点「版本号」7 次开启开发者选项；\n"
            "2) 开发者选项 → 打开「无线调试」；\n"
            "3) 点「使用配对码配对设备」，在 Termux 里执行 "
            "`adb pair 127.0.0.1:<配对端口>`，输入屏幕上的 6 位配对码；\n"
            "4) 回到无线调试页看「连接端口」，执行 `adb connect 127.0.0.1:<连接端口>`；\n"
            "5) `adb devices` 显示 device 后，重新运行 start_phone_service.sh。\n"
            "之后端口变化会自动用 adb mdns services 重连，不用再配对。"
            "（Android 11 以下没有无线调试，需用 LADB/Shizuku 或另备一台安卓机做 ADB host。）")


def wireless_reconnect():
    """无线调试端口每次开关都会变：用 mDNS / 缓存端口自动 adb connect 回本机。

    返回 (是否连上, 中文说明)。配对（adb pair）需要用户在系统弹窗里输一次码，
    无法也不应该自动化；但此后所有重连都不需要人再操作。
    """
    ds = [d for d in list_devices() if d["state"] == "device"]
    if ds:
        return True, f"已连接本机无线调试：{ds[0]['serial']}"
    ports = []
    forced = os.environ.get("XIAOZHI_ADB_PORT", "").strip()
    if forced.isdigit():
        ports.append(forced)
    try:
        out, _ = run(["mdns", "services"], timeout=10)
        ports += re.findall(r":(\d{4,5})", out)
    except Exception:
        pass
    port_file = Path.home() / ".xiaozhi_adb_port"
    try:
        cached = port_file.read_text().strip()
        if cached.isdigit():
            ports.append(cached)
    except Exception:
        pass
    for p in list(dict.fromkeys(ports)):
        try:
            out, _ = run(["connect", f"127.0.0.1:{p}"], timeout=8)
            if "connected" in out.lower():
                try:
                    port_file.write_text(p)
                except Exception:
                    pass
                time.sleep(0.5)
                ds = [d for d in list_devices() if d["state"] == "device"]
                if ds:
                    return True, f"已自动重连本机无线调试 127.0.0.1:{p}"
        except Exception:
            continue
    return False, termux_hint()


def setup_hint(devices=None):
    devices = devices if devices is not None else list_devices()
    if self_hosted():
        state_desc = ""
        if devices:
            state_desc = "\n当前 adb 看到的设备：" + "、".join(
                f"{d['serial']}({d['state']})" for d in devices)
            try:
                okflag, msg = wireless_reconnect()
                if okflag:
                    return msg
            except Exception:
                pass
        return termux_hint() + state_desc
    bad = [d for d in devices if d["state"] != "device"]
    if bad:
        d = bad[0]
        if d["state"] == "unauthorized":
            return ("手机已连上但还没授权 USB 调试：请看手机屏幕上弹出的"
                    "「允许 USB 调试吗？」并勾选「一律允许」，点确定；"
                    "若没弹窗，把 USB 线重插或在开发者选项里点「撤销 USB 调试授权」后重插。")
        if d["state"] == "offline":
            return "手机显示为 offline：请重新插拔 USB 数据线，确认线材支持数据传输（不是只能充电）。"
        return f"手机状态是 {d['state']}，请重插数据线或重新打开 USB 调试。"
    return ("现在没有检测到任何安卓手机。接入步骤（只需做一次）：\n"
            "1) 用数据线把手机接到这台 Mac；\n"
            "2) 手机：设置 → 关于手机 → 连点「版本号」7 次开启开发者选项；\n"
            "3) 设置 → 开发者选项 → 打开「USB 调试」，弹窗里点允许；\n"
            "4) 也可以免线：先在 USB 状态下让电脑执行 "
            "`adb tcpip 5555`，再 `adb connect 手机IP:5555`，之后同一 WiFi 即可。\n"
            "接好后再说一次这句话就行，不用重刷 ESP32。")


def list_devices():
    out, _ = run(["devices", "-l"], timeout=8)
    res = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line or line.startswith("*") or "List of devices" in line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        meta = {}
        for p in parts[2:]:
            if ":" in p:
                k, v = p.split(":", 1)
                meta[k] = v
        res.append({"serial": parts[0], "state": parts[1],
                    "model": meta.get("model", ""), "product": meta.get("product", ""),
                    "transport": meta.get("transport_id", "")})
    return res


def require_device():
    ds = list_devices()
    ok = [d for d in ds if d["state"] == "device"]
    if ok:
        return ok[0]
    if self_hosted():
        # 手机自托管：无线调试端口变了就自动重连，不要让用户重新配对。
        try:
            connected, _ = wireless_reconnect()
            if connected:
                ds = list_devices()
                ok = [d for d in ds if d["state"] == "device"]
                if ok:
                    return ok[0]
        except Exception:
            pass
    raise AdbError(setup_hint(ds))


def shell(cmd, serial=None, timeout=15):
    args = []
    if serial:
        args += ["-s", serial]
    args += ["shell", cmd]
    out, err = run(args, timeout=timeout)
    return out


def shell_ok(cmd, serial=None, timeout=15):
    return shell(cmd, serial, timeout).strip()


def screen_size(serial):
    out = shell_ok("wm size", serial)
    m = re.search(r"(\d{3,5})x(\d{3,5})", out)
    if not m:
        m = re.search(r"(\d{3,5})x(\d{3,5})", shell_ok("dumpsys display | grep mBaseDisplayInfo", serial))
    return [int(m.group(1)), int(m.group(2))] if m else [1080, 2400]


def current_app(serial):
    out = shell_ok("dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'", serial)
    m = re.search(r"([\w.]+)/([\w.$]+)", out)
    if m:
        return m.group(1), m.group(2)
    m = re.search(r"([\w.]+)/", out)
    return (m.group(1), "") if m else ("", "")


def _parse_bounds(s):
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", s or "")
    if not m:
        return None
    x1, y1, x2, y2 = [int(v) for v in m.groups()]
    return [x1, y1, x2, y2]


def dump_tree(serial, retries=2):
    """真实控件树。返回 (节点列表, 原始XML)。节点格式对齐 Open-AutoGLM。"""
    path = "/sdcard/xiaozhi_window.xml"
    cmd = f"uiautomator dump {path} >/dev/null 2>&1; cat {path}"
    raw = ""
    for _ in range(retries):
        raw = shell(cmd, serial, timeout=20)
        if "<hierarchy" in raw or "<node" in raw:
            break
        time.sleep(0.4)
    if "<hierarchy" not in raw and "<node" not in raw:
        raise AdbError("读不到手机界面控件树（uiautomator dump 没返回 XML）。"
                       "手机可能处于锁屏、正在播放动画或系统版本限制；"
                       "可以说「截屏识别」改用图片 OCR 读屏。")
    try:
        start = raw.find("<?xml")
        if start >= 0:
            end = raw.find("?>", start)
            raw = raw[end + 2:] if end > 0 else raw
        root = ET.fromstring(raw.strip())
    except Exception as e:
        raise AdbError(f"控件树 XML 解析失败：{e}")

    nodes = []
    def walk(el, depth=0):
        if el.tag == "node":
            a = el.attrib
            b = _parse_bounds(a.get("bounds", ""))
            text = (a.get("text") or "").strip()
            desc = (a.get("content-desc") or "").strip()
            if text or desc or a.get("clickable") == "true" or a.get("focused") == "true":
                nodes.append({
                    "text": text[:200],
                    "contentDescription": desc[:200],
                    "className": a.get("class", ""),
                    "resourceId": a.get("resource-id", ""),
                    "packageName": a.get("package", ""),
                    "bounds": a.get("bounds", ""),
                    "rect": b,
                    "center": [(b[0] + b[2]) // 2, (b[1] + b[3]) // 2] if b else None,
                    "clickable": a.get("clickable") == "true",
                    "scrollable": a.get("scrollable") == "true",
                    "focused": a.get("focused") == "true",
                    "enabled": a.get("enabled") != "false",
                    "depth": depth,
                })
        for c in el:
            walk(c, depth + 1)
    walk(root)
    return nodes, raw


def state(serial=None, want_ocr=False):
    """完整状态：控件树 + 前台应用 + 精简文字。这是「下一步怎么操作」的依据。"""
    serial = serial or require_device()["serial"]
    pkg, act = current_app(serial)
    size = screen_size(serial)
    try:
        nodes, _ = dump_tree(serial)
        tree_error = ""
    except AdbError as e:
        nodes, tree_error = [], str(e)
    lines = []
    for n in nodes:
        t = n["text"] or n["contentDescription"]
        if t and t not in lines:
            lines.append(t)
    ocr_lines, ocr_error = [], ""
    if want_ocr or not lines:
        try:
            ocr_lines = ocr_screen(serial)
            for t in ocr_lines:
                if t not in lines:
                    lines.append(t)
        except Exception as e:
            ocr_error = str(e)
    return {
        "serial": serial,
        "a11y_tree": nodes,
        "lines": lines[:120],
        "phone_state": {"currentApp": pkg, "packageName": pkg,
                        "activityName": act, "keyboardVisible": False},
        "screen": size,
        "tree_error": tree_error,
        "ocr_error": ocr_error,
    }


def brief(st, limit=1800):
    """给 ESP32 / 大模型看的精简屏幕文字，控制长度避免吃掉设备内存。"""
    pkg = st["phone_state"].get("packageName", "")
    head = f"手机当前前台：{pkg or '未知'}\n"
    lines = [f"{i+1}. {t}" for i, t in enumerate(st["lines"][:60])]
    if lines:
        body = "\n".join(lines)
    elif sys.platform == "darwin":
        body = "（控件树没读到文字，可以说「截屏识别」用图片 OCR）"
    else:
        body = "（控件树没读到文字：这个界面可能是自绘的，本机没有图片 OCR，请在手机上确认）"
    out = head + body
    if st.get("tree_error"):
        out += "\n[控件树] " + st["tree_error"][:120]
    return out[:limit]


# ---------------------------------------------------------------------------
# 应用：全部从这台手机实时读取，固件与 Portal 都不存名单
# ---------------------------------------------------------------------------

def _load_labels():
    try:
        return json.loads(LABELS_FILE.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _save_labels(d):
    try:
        LABELS_FILE.write_text(json.dumps(d, ensure_ascii=False, indent=1), encoding="utf-8")
    except Exception:
        pass


def learn_label(name, package, display=None):
    """把「用户说的名字」跟这台手机上真实读到的包名关联起来。

    这是运行时学习缓存（~/.xiaozhi_phone_labels.json），不是预置名单：
    固件、Portal 代码里都没有任何 App 映射，删掉这个文件会自动重新学习。
    """
    if not name or not package:
        return
    d = _load_labels()
    info = d.setdefault(package, {})
    if display and not info.get("name"):
        info["name"] = display
    aliases = info.setdefault("aliases", [])
    if name not in aliases:
        aliases.insert(0, name)
        info["aliases"] = aliases[:8]
        info["ts"] = time.time()
        _save_labels(d)


def launcher_icons(serial, go_home=True):
    """读桌面/应用抽屉上真实显示的图标文字 —— 这才是手机里「看得见的应用名」。

    Android 的 pm/cmd 只给包名（com.tencent.mm），中文名只存在于 launcher 的
    图标文字里。所以从控件树实时读，不靠任何写死的映射表。
    """
    w, h = screen_size(serial)
    if go_home:
        shell("input keyevent 3", serial)
        time.sleep(0.4)
    st = state(serial)

    def pick(s):
        return [n for n in s["a11y_tree"]
                if n.get("clickable") and n.get("center")
                and (n["text"] or n["contentDescription"])
                and "com.android.systemui" not in (n.get("packageName") or "")]

    icons = pick(st)
    if not icons:
        # 有些 launcher 要点「应用抽屉」或上滑才显示图标
        shell(f"input swipe {w // 2} {int(h * .82)} {w // 2} {int(h * .25)} 400", serial)
        time.sleep(0.5)
        st = state(serial)
        icons = pick(st)
    return icons, st


def launcher_names(serial, go_home=True):
    """读桌面上真实显示的中文应用名并缓存，供「手机里装了什么」使用。

    只缓存从这台手机读到的文字，不是预置名单；换一台手机重跑引导会重建。
    """
    icons, _ = launcher_icons(serial, go_home=go_home)
    names = list(dict.fromkeys(
        (n["text"] or n["contentDescription"]).strip() for n in icons
        if (n["text"] or n["contentDescription"]).strip()))[:60]
    if names:
        d = _load_labels()
        d["__launcher__"] = {"names": names, "ts": time.time()}
        _save_labels(d)
    return names


def cached_launcher_names():
    return _load_labels().get("__launcher__", {}).get("names", [])


def _similar(a, b):
    import difflib
    a, b = (a or "").lower().replace(" ", ""), (b or "").lower().replace(" ", "")
    if not a or not b:
        return 0.0
    if a == b or a in b or b in a:
        return 1.0
    return difflib.SequenceMatcher(None, a, b).ratio()


def smart_open(query, serial):
    """按用户在桌面上看到的名字找应用，点开它，并从前台回读真实包名完成学习。

    好处：零预置名单、零副作用（本来用户就是要打开这个 App）、
    不用把几十 MB 的 APK 拖回电脑解析。首次说过一次之后就写进学习缓存。
    """
    icons, st = launcher_icons(serial)
    if not icons:
        raise AdbError("在手机桌面上没读到应用图标文字。可以说「截屏识别」用 OCR 再试，"
                       "或直接说包名，例如 com.tencent.mm。")
    ranked = sorted(((_similar(query, n["text"] or n["contentDescription"]), n)
                     for n in icons), key=lambda x: -x[0])
    score, icon = ranked[0]
    if score < 0.5:
        names = "、".join((n["text"] or n["contentDescription"]) for n in icons[:30])
        raise AdbError(f"这台手机桌面上没找到「{query}」。桌面可见应用：{names}")
    label = icon["text"] or icon["contentDescription"]
    x, y = icon["center"]
    shell(f"input tap {x} {y}", serial)
    time.sleep(1.0)
    pkg, _ = current_app(serial)
    if not pkg or "launcher" in pkg or "home" in pkg:
        raise AdbError(f"点了桌面上的「{label}」，但前台应用没变化（可能弹了权限框或还在加载）。"
                       f"可以说「读一下手机屏幕」看看现在显示什么。")
    apps = installed_apps(serial)
    app = next((dict(a) for a in apps if a["package"] == pkg), None)
    learn_label(query, pkg, display=label)
    learn_label(label, pkg, display=label)
    if app:
        app["name"] = label
        app["aliases"] = [query, label]
    return {"app": app or {"name": label, "package": pkg, "activity": "", "aliases": [query]},
            "launched": True, "foreground": True,
            "hint": f"（首次从桌面动态学到：{label} → {pkg}）"}


def launcher_activities(serial):
    """一次拿到所有可启动的 launcher Activity（Android 10+）。失败时返回空字典。"""
    out = shell_ok("cmd package query-activities --brief -a android.intent.action.MAIN "
                   "-c android.intent.category.LAUNCHER", serial, timeout=20)
    acts = {}
    for line in out.splitlines():
        line = line.strip()
        m = re.match(r"([\w.]+)/([\w.$]+)", line)
        if m and "/" in line:
            acts.setdefault(m.group(1), m.group(2))
    return acts


def installed_apps(serial=None, refresh=False):
    serial = serial or require_device()["serial"]
    out = shell_ok("pm list packages -3", serial)
    pkgs = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("package:"):
            p = line[8:].strip()
            if p:
                pkgs.append(p)
    acts = launcher_activities(serial)
    labels = _load_labels()
    apps = []
    for p in pkgs:
        info = labels.get(p, {})
        name = info.get("name") or p.split(".")[-1]
        apps.append({
            "name": name,
            "package": p,
            "activity": acts.get(p, ""),
            "aliases": info.get("aliases", []),
        })
    return apps


def match_app(query, apps):
    q = (query or "").strip().lower().replace(" ", "")
    if not q:
        return None
    def score(a):
        name = (a["name"] or "").lower().replace(" ", "")
        pkg = a["package"].lower()
        al = [x.lower().replace(" ", "") for x in a.get("aliases", [])]
        if q == name or q == pkg or q in al:
            return 100
        if q in name or q in pkg:
            return 80 - abs(len(name) - len(q))
        if any(q in x or x in q for x in al):
            return 70
        last = pkg.split(".")[-1]
        if q in last or last in q:
            return 50
        return 0
    ranked = sorted(((score(a), a) for a in apps), key=lambda x: -x[0])
    return ranked[0][1] if ranked and ranked[0][0] >= 50 else None


def open_app(query, serial=None):
    serial = serial or require_device()["serial"]
    apps = installed_apps(serial)
    app = match_app(query, apps)
    if not app:
        # 包名匹配不上时，去手机桌面读真实图标文字，点开并回读包名完成学习。
        # 绝不靠代码里写死「微信=com.tencent.mm」这类映射。
        return smart_open(query, serial)
    act = app["activity"]
    cmd = (f"am start -n {app['package']}/{act}" if act
           else f"monkey -p {app['package']} -c android.intent.category.LAUNCHER 1")
    out = shell(cmd, serial, timeout=15)
    if "Error" in out and "Warning" not in out:
        out = shell(f"monkey -p {app['package']} -c android.intent.category.LAUNCHER 1",
                    serial, timeout=15)
    time.sleep(0.8)
    pkg, _ = current_app(serial)
    ok = pkg == app["package"]
    learn_label(query, app["package"] if ok else "")
    return {"app": app, "launched": bool("Error" not in out), "foreground": ok,
            "hint": "" if ok else f"已发出启动命令，但前台仍是 {pkg or '未知'}，"
                                  f"可能需要先在手机上解锁或关闭弹窗。"}


# ---------------------------------------------------------------------------
# 输入：中文靠 ADBKeyboard，自动引导安装并启用（不需要 root）
# ---------------------------------------------------------------------------

def bootstrap(serial=None, install=True):
    """自动引导接入：检测设备 → 装/启用中文输入法 → 读一次屏幕和应用。"""
    serial = serial or require_device()["serial"]
    steps = []
    model = shell_ok("getprop ro.product.model", serial)
    brand = shell_ok("getprop ro.product.brand", serial)
    ver = shell_ok("getprop ro.build.version.release", serial)
    steps.append(f"设备：{brand} {model} Android {ver}（{serial}）")
    imes = shell_ok("ime list -s", serial)
    if ADB_IME not in imes:
        if install and KEYBOARD_APK.exists():
            r = subprocess.run([adb_bin(), "-s", serial, "install", "-r", str(KEYBOARD_APK)],
                               capture_output=True, timeout=90)
            msg = r.stdout.decode("utf-8", "replace").strip()
            steps.append(f"已自动安装 ADBKeyboard：{msg[-120:]}")
            imes = shell_ok("ime list -s", serial)
        elif install:
            steps.append("未找到 tools/ADBKeyboard.apk：中文输入暂不可用，"
                         "把 ADBKeyboard.apk 放到 tools/ 后说「手机接入引导」即可自动装。")
    if ADB_IME in imes or (install and KEYBOARD_APK.exists()):
        shell_ok(f"ime enable {ADB_IME}", serial)
        shell_ok(f"ime set {ADB_IME}", serial)
        steps.append("已自动切换 ADBKeyboard 输入法（中文可直接输入）")
    else:
        steps.append("中文输入降级为 adb input text（只支持英文/数字）")
    try:
        st = state(serial)
        steps.append(f"读屏成功：{len(st['a11y_tree'])} 个控件，"
                     f"{len(st['lines'])} 条文字，前台 {st['phone_state']['packageName']}")
    except Exception as e:
        steps.append(f"读屏检查：{e}")
    apps = installed_apps(serial)
    steps.append(f"动态读到 {len(apps)} 个已安装应用：" +
                 "、".join(a["name"] for a in apps[:20]))
    try:
        names = launcher_names(serial, go_home=True)
        steps.append("桌面可见的中文应用名：" + "、".join(names[:30]))
    except Exception as e:
        steps.append(f"桌面应用名读取：{str(e)[:120]}")
    return "\n".join(steps)


def input_text(text, serial=None):
    serial = serial or require_device()["serial"]
    if not text:
        raise AdbError("要输入的文字是空的")
    imes = shell_ok("ime list -s", serial)
    if ADB_IME in imes or KEYBOARD_APK.exists():
        # ADBKeyboard 的广播方式，中文/emoji 都走 UTF-8，不受 adb input text 的 ASCII 限制
        b64 = base64.b64encode(text.encode("utf-8")).decode()
        r = shell(f"am broadcast -a ADB_INPUT_B64 --es msg {b64}", serial)
        if "result=-1" not in r and "Broadcast completed" not in r:
            q = shlex.quote(text)
            r = shell(f"am broadcast -a ADB_INPUT_TEXT --es msg {q}", serial)
        return {"method": "ADBKeyboard", "raw": r.strip()[:160]}
    if re.fullmatch(r"[\x20-\x7e]+", text):
        shell(f"input text {shlex.quote(text)}", serial)
        return {"method": "input text", "raw": ""}
    raise AdbError("手机还没装 ADBKeyboard，中文没法直接输入。"
                   "说「手机接入引导」，电脑会自动安装并启用；或先说英文内容。")


# ---------------------------------------------------------------------------
# 图片 OCR：adb 原生像素 + macOS Vision，本地完成，不上传任何截图
# ---------------------------------------------------------------------------

def screenshot(serial=None):
    serial = serial or require_device()["serial"]
    data, err = run(["-s", serial, "exec-out", "screencap", "-p"], timeout=25, binary=True)
    if not data.startswith(b"\x89PNG"):
        data = data.replace(b"\r\n", b"\n")
    if not data.startswith(b"\x89PNG"):
        raise AdbError("截屏失败（手机可能锁屏或不支持 screencap）：" + err.decode("utf-8", "replace")[:120])
    return data


def ocr_screen(serial=None, png=None):
    """Vision 本地 OCR。返回带坐标的文字行，用于 uiautomator 读不到的自绘界面。

    手机自托管（Termux）没有 macOS Vision 框架：不伪造识别结果，直接返回空，
    由 state() 如实标注「图片 OCR 不可用」，主链路仍靠真实控件树工作。
    """
    if sys.platform != "darwin":
        return []
    try:
        import objc
        import Quartz
        from Foundation import NSData
        bundle = objc.pathForFramework("/System/Library/Frameworks/Vision.framework")
        if not bundle:
            return []
        ns = {}
        objc.loadBundle("Vision", ns, bundle_path=bundle)
        VNRecognizeTextRequest = ns["VNRecognizeTextRequest"]
        VNImageRequestHandler = ns["VNImageRequestHandler"]
        data = png if png is not None else screenshot(serial)
        nsd = NSData.dataWithBytes_length_(data, len(data))
        src = Quartz.CGImageSourceCreateWithData(nsd, None)
        cg = Quartz.CGImageSourceCreateImageAtIndex(src, 0, None)
        if cg is None:
            return []
        handler = VNImageRequestHandler.alloc().initWithCGImage_options_(cg, {})
        req = VNRecognizeTextRequest.alloc().init()
        try:
            req.setRecognitionLanguages_(["zh-Hans", "en-US"])
        except Exception:
            pass
        try:
            req.setRecognitionLevel_(0)
        except Exception:
            pass
        ok, _ = handler.performRequests_error_([req], None)
        if not ok:
            return []
        lines = []
        for obs in (req.results() or []):
            cands = obs.topCandidates_(1)
            if cands and len(cands):
                s = str(cands[0].string())
                if s.strip():
                    lines.append(s.strip())
        return lines[:100]
    except Exception as e:
        raise AdbError(f"本地 OCR 不可用：{e}")


# ---------------------------------------------------------------------------
# 原子动作 + 敏感动作二次确认
# ---------------------------------------------------------------------------

def tap_node(nodes, target, index=1):
    """在当前屏幕上按文字找控件并点它 —— 不写死坐标，全靠实时回读。"""
    q = (target or "").strip().lower()
    cand = []
    for n in nodes:
        t = (n["text"] or n["contentDescription"]).strip()
        if not t or not n.get("center"):
            continue
        if q:
            if q in t.lower() or t.lower() in q:
                cand.append(n)
        elif n.get("clickable"):
            cand.append(n)
    if not cand:
        raise AdbError(f"当前屏幕上没找到可以点的「{target or '按钮'}」。"
                       f"屏幕文字：{'、'.join(n['text'] or n['contentDescription'] for n in nodes[:20])}")
    i = max(0, min(len(cand) - 1, index - 1))
    n = cand[i]
    return n, f"点击「{(n['text'] or n['contentDescription'])[:40]}」@({n['center'][0]},{n['center'][1]})"


def _need_confirm(action, payload):
    import hashlib
    token = hashlib.md5(f"{action}{time.time()}{payload}".encode()).hexdigest()[:8]
    _confirm.update(token=token, action=action, payload=payload, ts=time.time())
    return token


def check_confirm(token, action):
    return (token and token == _confirm["token"]
            and action == _confirm["action"]
            and time.time() - _confirm["ts"] < 300)


def act(action, serial=None, confirmed=False, **kw):
    """执行一个原子动作，并回读屏幕。返回 (结果文字, 状态dict)。"""
    serial = serial or require_device()["serial"]
    w, h = screen_size(serial)
    act_name = (action or "state").strip().lower()
    detail = ""

    # 二次确认：用户说出确认码后，重新执行刚才挂起的那个敏感动作。
    if act_name == "confirm_execute":
        token = kw.get("confirm") or ""
        if not (token and token == _confirm["token"]
                and time.time() - _confirm["ts"] < 300):
            raise AdbError("没有待确认的手机操作，或确认码已过期（5 分钟）。请重新说一次要做什么。")
        original, payload = _confirm["action"], {}
        try:
            payload = json.loads(_confirm["payload"] or "{}")
        except Exception:
            pass
        _confirm.update(token="", action="", payload=None, ts=0)
        return act(original, serial, confirmed=True, **payload)

    if act_name in FORBID_AUTO:
        raise AdbError("支付、密码、验证码这类动作不自动执行。我可以把页面上的金额、"
                       "商品和按钮文字读给你，最后由你本人在手机上确认。")
    if act_name in SENSITIVE and not confirmed:
        t = _need_confirm(act_name, json.dumps(kw, ensure_ascii=False, default=str)[:400])
        st0 = state(serial)
        return (f"这是个敏感操作（{act_name}），需要你再确认一次。"
                f"当前屏幕：{brief(st0, 500)}\n"
                f"确认请说「确认执行 {t}」，或原话再说一遍并带上确认令牌 {t}。",
                {"need_confirm": True, "token": t})

    if act_name in ("state", "read", "screen"):
        pass
    elif act_name == "apps":
        apps = installed_apps(serial)
        return ("这台手机真实安装的应用（实时读取）：" +
                "、".join(a["name"] for a in apps[:60]), state(serial))
    elif act_name == "open":
        r = open_app(kw.get("query") or kw.get("text") or "", serial)
        detail = f"已打开「{r['app']['name']}」({r['app']['package']})。" + r["hint"]
    elif act_name == "tap":
        x, y = int(kw.get("x", 0)), int(kw.get("y", 0))
        if x <= 1000 and y <= 1000 and (x or y):
            x, y = int(x * w / 1000), int(y * h / 1000)
        shell(f"input tap {int(x)} {int(y)}", serial)
        detail = f"点击 ({x},{y})"
    elif act_name in ("tap_text", "click"):
        st0 = state(serial)
        n, detail = tap_node(st0["a11y_tree"], kw.get("target") or kw.get("text") or "",
                             int(kw.get("index", 1) or 1))
        x, y = n["center"]
        shell(f"input tap {x} {y}", serial)
    elif act_name == "swipe":
        d = (kw.get("direction") or "up").lower()
        pairs = {"up": (w // 2, int(h * .75), w // 2, int(h * .3)),
                 "down": (w // 2, int(h * .3), w // 2, int(h * .75)),
                 "left": (int(w * .8), h // 2, int(w * .2), h // 2),
                 "right": (int(w * .2), h // 2, int(w * .8), h // 2)}
        x1, y1, x2, y2 = pairs.get(d, pairs["up"])
        if kw.get("startX") is not None:
            x1, y1, x2, y2 = (int(kw.get(k, 0)) for k in ("startX", "startY", "endX", "endY"))
        shell(f"input swipe {x1} {y1} {x2} {y2} 500", serial)
        detail = f"滑动 {d} ({x1},{y1})→({x2},{y2})"
    elif act_name in ("input", "type", "text"):
        r = input_text(kw.get("text") or kw.get("query") or "", serial)
        detail = f"输入「{(kw.get('text') or '')[:40]}」方式={r['method']}"
    elif act_name == "enter":
        shell("input keyevent 66", serial)
        detail = "已按回车/搜索键"
    elif act_name == "back":
        shell("input keyevent 4", serial)
        detail = "已按返回键"
    elif act_name == "home":
        shell("input keyevent 3", serial)
        detail = "已回到桌面"
    elif act_name == "key":
        shell(f"input keyevent {int(kw.get('key', 66))}", serial)
        detail = f"已按键 keycode={kw.get('key')}"
    elif act_name == "ocr":
        lines = ocr_screen(serial)
        detail = "图片 OCR 读到：" + "、".join(lines[:40])
        return detail, state(serial, want_ocr=False)
    elif act_name == "bootstrap":
        return bootstrap(serial), state(serial)
    elif act_name in ("message", "send", "reply", "order", "confirm_execute"):
        # 二次确认后：按屏幕上的文字点「发送 / 提交订单」按钮，不构造任何绕过逻辑
        st0 = state(serial)
        target = kw.get("target") or ("发送" if act_name != "order" else "提交订单")
        n, detail = tap_node(st0["a11y_tree"], target)
        x, y = n["center"]
        shell(f"input tap {x} {y}", serial)
        detail = "已" + detail + "（你确认过的动作）"
    else:
        raise AdbError(f"不认识的手机动作：{act_name}")

    time.sleep(0.6)
    st = state(serial)
    result = (detail + "\n" if detail else "") + brief(st)
    return result, st


if __name__ == "__main__":
    import sys
    a = sys.argv[1] if len(sys.argv) > 1 else "state"
    if a == "devices":
        print(json.dumps(list_devices(), ensure_ascii=False, indent=1))
    elif a == "bootstrap":
        print(bootstrap())
    else:
        text, st = act(a, **dict(x.split("=", 1) for x in sys.argv[2:] if "=" in x))
        print(text)
