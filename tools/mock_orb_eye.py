#!/usr/bin/env python3
"""模拟一台装了 orb-eye 无障碍服务的安卓手机（:7333）。

用途：在没有真机时，让 ESP32 固件把这台 Mac 当成手机连进来，端到端验证
「语音整句 → 固件协议桥 → 控件树 → 点击/输入/回车 → 真实回读」全链路。
接口契约与 github.com/EatTake/orb-eye v2.0 完全一致。

    python3 mock_orb_eye.py 7333
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"screen": "home", "typed": "", "log": [], "page": 0}


def nodes_for(screen):
    def n(text, l, t, r, b, click=False, edit=False):
        return {"text": text, "clickable": click, "editable": edit,
                "bounds": "%d %d %d %d" % (l, t, r, b)}

    if screen == "home":
        items = ["美团", "微信", "抖音", "QQ音乐", "Chrome 浏览器", "设置", "相机"]
        out = [n("桌面", 0, 0, 1080, 120)]
        for i, name in enumerate(items):
            x = 100 + (i % 4) * 250
            y = 300 + (i // 4) * 320
            out.append(n(name, x, y, x + 180, y + 200, click=True))
        return out
    if screen == "meituan":
        return [n("美团外卖", 0, 40, 1080, 140),
                n("搜索", 80, 180, 1000, 300, click=True, edit=True),
                n("奶茶", 80, 380, 500, 470, click=True),
                n("附近商家", 80, 520, 500, 610, click=True)]
    if screen == "meituan_result":
        typed = STATE["typed"] or "奶茶"
        return [n("搜索: " + typed, 0, 40, 1080, 150),
                n("喜茶 多肉葡萄 18 元", 60, 220, 1000, 340, click=True),
                n("奈雪 霸气芝士 22 元", 60, 380, 1000, 500, click=True),
                n("蜜雪冰城 珍珠奶茶 7 元", 60, 540, 1000, 660, click=True),
                n("搜索", 800, 760, 1040, 860, click=True)]
    if screen == "wechat":
        return [n("微信", 0, 40, 1080, 140),
                n("搜索", 80, 180, 1000, 290, click=True, edit=True),
                n("张三", 60, 340, 1000, 450, click=True),
                n("李四", 60, 480, 1000, 590, click=True)]
    if screen == "wechat_chat":
        return [n("张三", 0, 40, 1080, 140),
                n("输入消息", 60, 1900, 900, 2050, edit=True),
                n("发送", 900, 1900, 1050, 2050, click=True)]
    return [n(screen, 0, 0, 1080, 140)]


def ok(**kw):
    d = {"ok": True}
    d.update(kw)
    return d


def err(msg):
    return {"ok": False, "error": msg}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        # 真机 orb-eye 发完就 client.close()，这里保持一致
        self.send_header("Connection", "close")
        self.close_connection = True
        self.end_headers()
        self.wfile.write(body)

    def _route(self, method, path, raw):
        route = path.split("?")[0]
        STATE["log"].append({"t": round(time.time() - T0, 2), "m": method,
                             "p": route, "b": raw[:200]})
        if route == "/ping":
            return ok(service="orb-eye", version="2.0")
        if route == "/screen":
            return {"elements": nodes_for(STATE["screen"])}
        if route == "/tree":
            return {"text": STATE["screen"], "nodes": nodes_for(STATE["screen"])}
        if route == "/info":
            return ok(package={"home": "com.android.launcher",
                               "meituan": "com.meituan.android",
                               "wechat": "com.tencent.mm"}.get(STATE["screen"], "com.demo"),
                      windowPackage="com.demo", activity="MainActivity")
        if route == "/home":
            STATE["screen"] = "home"
            STATE["typed"] = ""
            return ok()
        if route == "/back":
            STATE["screen"] = "home"
            return ok()
        if route == "/setText":
            try:
                STATE["typed"] = json.loads(raw or "{}").get("text", "")
                return ok(text=STATE["typed"])
            except Exception as e:
                return err(str(e))
        if route == "/click":
            try:
                target = json.loads(raw or "{}").get("text", "")
            except Exception:
                target = ""
            if target in ("美团", "外卖"):
                STATE["screen"] = "meituan"
                return ok(clicked=True, matchedText=target)
            if target in ("微信",):
                STATE["screen"] = "wechat"
                return ok(clicked=True, matchedText=target)
            if target in ("张三", "李四"):
                STATE["screen"] = "wechat_chat"
                return ok(clicked=True, matchedText=target)
            if target in ("搜索", "回车", "Enter", "确定", "前往"):
                if STATE["screen"] == "meituan":
                    STATE["screen"] = "meituan_result"
                return ok(clicked=True, matchedText=target)
            if target == "发送":
                STATE["sent"] = STATE["typed"]
                return ok(clicked=True, matchedText=target, sent=STATE["typed"])
            for it in nodes_for(STATE["screen"]):
                if target and target in it.get("text", ""):
                    return ok(clicked=True, matchedText=it["text"])
            return err("No matching node: " + target)
        if route == "/tap":
            try:
                x = int(json.loads(raw or "{}").get("x", 0))
                y = int(json.loads(raw or "{}").get("y", 0))
            except Exception:
                x = y = 0
            for it in nodes_for(STATE["screen"]):
                l, t, r, b = [int(v) for v in it["bounds"].split()]
                if l <= x <= r and t <= y <= b:
                    name = it["text"]
                    if name in ("美团", "微信"):
                        STATE["screen"] = "meituan" if name == "美团" else "wechat"
                    elif name in ("张三", "李四"):
                        STATE["screen"] = "wechat_chat"
                    return ok(x=x, y=y, matchedText=name)
            return ok(x=x, y=y)
        if route == "/swipe":
            STATE["page"] += 1
            return ok(dispatched=True)
        if route == "/log":
            return {"log": STATE["log"][-60:]}
        if route == "/reset":
            STATE.update(screen="home", typed="", log=[], page=0)
            return ok()
        return err("Unknown route: " + route)

    def do_GET(self):
        self._send(self._route("GET", self.path, ""))

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        self._send(self._route("POST", self.path, raw))


T0 = time.time()
if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7333
    srv = ThreadingHTTPServer(("0.0.0.0", port), H)
    print("mock orb-eye 手机已启动: 0.0.0.0:%d" % port, flush=True)
    srv.serve_forever()
