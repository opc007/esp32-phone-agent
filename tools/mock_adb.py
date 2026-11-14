#!/usr/bin/env python3
"""mock_adb.py —— 没有真机时模拟一台安卓手机，用来端到端自测 Portal→手机执行器链路。

用法：XIAOZHI_ADB=/path/to/mock_adb.py python3 tools/computer_portal.py 8081 test
状态存在 ~/.xiaozhi_mock_adb.json，可用 `rm` 删掉复位（不会碰任何真实文件）。
"""
import json
import os
import sys
from pathlib import Path

S = Path.home() / ".xiaozhi_mock_adb.json"
SERIAL = "MOCK0A1B2C3D"


def load():
    if S.exists():
        try:
            return json.loads(S.read_text())
        except Exception:
            pass
    return {"app": "com.miui.home", "screen": "home", "query": "", "text": ""}


def save(d):
    S.write_text(json.dumps(d, ensure_ascii=False))


def node(i, text, desc, cls="android.widget.TextView", clickable="false", bounds="[0,0][100,100]", res=""):
    return (f'<node index="{i}" text="{text}" resource-id="{res}" class="{cls}" '
            f'package="com.miui.home" content-desc="{desc}" checkable="false" checked="false" '
            f'clickable="{clickable}" enabled="true" focusable="false" focused="false" '
            f'scrollable="false" long-clickable="false" password="false" selected="false" '
            f'bounds="{bounds}" />')


def hierarchy(d):
    n = []
    if d["screen"] == "home":
        n += [node(0, "美团", "美团", "android.widget.TextView", "true", "[40,300][220,470]"),
              node(1, "抖音", "抖音", "android.widget.TextView", "true", "[260,300][440,470]"),
              node(2, "微信", "微信", "android.widget.TextView", "true", "[480,300][660,470]"),
              node(3, "QQ音乐", "QQ音乐", "android.widget.TextView", "true", "[700,300][880,470]")]
    elif d["screen"] == "meituan":
        n += [node(0, "搜索奶茶、外卖、美食", "", "android.widget.EditText", "true", "[60,140][1020,240]", "com.meituan:id/search"),
              node(1, "喜茶 多肉葡萄 18 元", "", "android.widget.TextView", "true", "[40,500][1040,620]"),
              node(2, "奈雪的茶 杨枝甘露 22 元", "", "android.widget.TextView", "true", "[40,640][1040,760]"),
              node(3, "蜜雪冰城 珍珠奶茶 7 元", "", "android.widget.TextView", "true", "[40,780][1040,900]"),
              node(4, "去结算", "", "android.widget.Button", "true", "[400,2100][680,2260]")]
    elif d["screen"] == "douyin":
        n += [node(0, "奶茶探店 一周打卡 5 家", "", "android.widget.TextView", "true", "[40,300][1040,420]"),
              node(1, "说点什么...", "", "android.widget.EditText", "true", "[60,2180][800,2300]"),
              node(2, "发送", "", "android.widget.Button", "true", "[840,2180][1040,2300]")]
    elif d["screen"] == "wechat":
        n += [node(0, "微信", "", "android.widget.TextView", "false", "[40,100][300,180]"),
              node(1, "文件传输助手", "", "android.widget.TextView", "true", "[40,300][1000,420]"),
              node(2, "张三", "", "android.widget.TextView", "true", "[40,440][1000,560]")]
    if d.get("text"):
        n.append(node(90, "已输入：" + d["text"], "", "android.widget.TextView", "false", "[40,1000][1000,1100]"))
    if d.get("query"):
        n.append(node(91, "搜索结果：" + d["query"] + " 最低 7 元起", "",
                      "android.widget.TextView", "true", "[40,1200][1000,1320]"))
    return '<?xml version="1.0" encoding="UTF-8"?>\n<hierarchy rotation="0">\n' + "\n".join(n) + "\n</hierarchy>"


def main():
    a = sys.argv[1:]
    if a and a[0] == "devices":
        print("List of devices attached")
        print(f"{SERIAL}       device product:mock model:Xiaomi_14 device:mock transport_id:1")
        return
    if a and a[0] == "exec-out":
        # 返回一张 1x1 PNG，OCR 会优雅降级；真实设备这里返回原生分辨率截图
        sys.stdout.buffer.write(bytes.fromhex(
            "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
            "0000000d49444154789c6360000002000100ffff03000006000557bfabd400000000"
            "49454e44ae426082"))
        return
    # -s <serial> shell <cmd>
    cmd = a[-1] if a else ""
    d = load()
    if cmd.startswith("uiautomator dump"):
        print(hierarchy(d))
    elif cmd.startswith("pm list packages"):
        print("package:com.meituan.android\npackage:com.ss.android.ugc.aweme\n"
              "package:com.tencent.mm\npackage:com.tencent.qqmusic")
    elif cmd.startswith("cmd package query-activities"):
        print("com.meituan.android/com.meituan.android.pt.home.HomeActivity\n"
              "com.ss.android.ugc.aweme/com.ss.android.ugc.aweme.splash.SplashActivity\n"
              "com.tencent.mm/com.tencent.mm.ui.LauncherUI\n"
              "com.tencent.qqmusic/com.tencent.qqmusic.activity.AppStarterActivity")
    elif cmd.startswith("dumpsys window"):
        print(f"  mCurrentFocus=Window{{abc u0 {d['app']}/{d['app']}.MainActivity}}")
    elif cmd.startswith("wm size"):
        print("Physical size: 1080x2400")
    elif cmd.startswith("dumpsys display"):
        print("mBaseDisplayInfo=DisplayInfo{..., 1080 x 2400}")
    elif cmd.startswith("ime list"):
        print("com.android.adbkeyboard/.AdbIME\ncom.miui.securitycenter/.MiuiKeyboard")
    elif cmd.startswith("ime "):
        print("")
    elif cmd.startswith("getprop ro.product.brand"):
        print("Xiaomi")
    elif cmd.startswith("getprop ro.product.model"):
        print("Xiaomi 14 (mock)")
    elif cmd.startswith("getprop ro.build.version.release"):
        print("14")
    elif cmd.startswith("am start") or cmd.startswith("monkey"):
        if "com.meituan" in cmd:
            d.update(app="com.meituan.android", screen="meituan")
        elif "aweme" in cmd:
            d.update(app="com.ss.android.ugc.aweme", screen="douyin")
        elif "tencent.mm" in cmd:
            d.update(app="com.tencent.mm", screen="wechat")
        else:
            d.update(app="com.tencent.qqmusic", screen="home")
        save(d)
        print("Starting: Intent { cmp=... }")
    elif cmd.startswith("am broadcast"):
        import re
        m = re.search(r"--es msg (\S+)", cmd)
        raw = m.group(1).strip("'\"") if m else ""
        try:
            import base64
            d["text"] = base64.b64decode(raw).decode("utf-8")
        except Exception:
            d["text"] = raw
        save(d)
        print("Broadcast completed: result=-1")
    elif cmd.startswith("input text"):
        d["text"] = cmd.split("input text", 1)[1].strip().strip("'")
        save(d)
        print("")
    elif cmd.startswith("input keyevent 66"):
        d["query"] = d.get("text", "")
        save(d)
        print("")
    elif cmd.startswith("input keyevent 3"):
        d.update(app="com.miui.home", screen="home")
        save(d)
        print("")
    elif cmd.startswith("input keyevent 4"):
        d.update(app="com.miui.home", screen="home")
        save(d)
        print("")
    elif cmd.startswith("input tap"):
        import re
        m = re.search(r"input tap (\d+) (\d+)", cmd)
        x, y = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        d["last_tap"] = cmd
        if d["screen"] == "home" and 300 <= y <= 470:
            if 40 <= x <= 220:
                d.update(app="com.meituan.android", screen="meituan")
            elif 260 <= x <= 440:
                d.update(app="com.ss.android.ugc.aweme", screen="douyin")
            elif 480 <= x <= 660:
                d.update(app="com.tencent.mm", screen="wechat")
            elif 700 <= x <= 880:
                d.update(app="com.tencent.qqmusic", screen="home")
        save(d)
        print("")
    elif cmd.startswith("input swipe"):
        d["last_swipe"] = cmd
        save(d)
        print("")
    else:
        print("")


if __name__ == "__main__":
    main()
