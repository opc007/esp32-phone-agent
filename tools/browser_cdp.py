#!/usr/bin/env python3
"""
browser_cdp.py —— 用 Chrome DevTools Protocol 真实操作浏览器。

为什么需要它：
  macOS 辅助功能树只能读到 Chrome 的窗口/标签栏骨架，网页 DOM（搜索结果、
  播放按钮、正文）完全读不到；osascript 又被 TCC 拒绝。CDP 让我们能：
    打开浏览器 / 新标签 -> 在搜索框输入中文 -> 真实按回车 ->
    读回页面文字 -> 按文字点链接和按钮 -> 读播放状态。
  全部是真实浏览器行为，不是模拟返回。

实现：纯 Python 标准库（socket 手写 WebSocket 客户端），无需 pip 安装。
会启动一个独立的 Chrome 实例（独立 user-data-dir + 9222 调试端口），
不触碰用户原有的一堆标签页。
"""
import base64
import json
import os
import socket
import struct
import subprocess
import time
import urllib.parse
import urllib.request

PORT = 9222
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
PROFILE = os.path.expanduser("~/Library/Application Support/XiaozhiBrowser")
BASE = f"http://127.0.0.1:{PORT}"

# 本机环境设了 HTTP_PROXY，会把 127.0.0.1:9222 也送进代理并返回 502。
# CDP 是本地回环，必须显式绕过代理。
_NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _http_json(path, method="GET", timeout=3):
    req = urllib.request.Request(BASE + path, method=method)
    with _NO_PROXY.open(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


class _WS:
    """最小 WebSocket 客户端：握手 + 文本帧收发（含 masking / ping / 分片）。"""

    def __init__(self, url):
        u = urllib.parse.urlparse(url)
        self.sock = socket.create_connection((u.hostname, u.port or 80), timeout=6)
        self.sock.settimeout(6)
        self.rest = b""
        key = base64.b64encode(os.urandom(16)).decode()
        path = u.path + (("?" + u.query) if u.query else "")
        req = (f"GET {path} HTTP/1.1\r\n"
               f"Host: {u.hostname}:{u.port}\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = self.sock.recv(4096)
            if not d:
                raise RuntimeError("WebSocket 握手被关闭")
            buf += d
        head, self.rest = buf.split(b"\r\n\r\n", 1)
        if b"101" not in head.split(b"\r\n", 1)[0]:
            raise RuntimeError("WebSocket 握手失败: " + head[:180].decode("utf-8", "replace"))
        self._id = 0

    def _exact(self, n):
        out = bytearray()
        if self.rest:
            take, self.rest = self.rest[:n], self.rest[n:]
            out.extend(take)
        while len(out) < n:
            d = self.sock.recv(n - len(out))
            if not d:
                raise RuntimeError("WebSocket 连接断开")
            out.extend(d)
        return bytes(out)

    def _send(self, data, opcode=1):
        payload = data.encode("utf-8") if isinstance(data, str) else data
        n = len(payload)
        hdr = bytearray([0x80 | opcode])
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126)
            hdr.extend(struct.pack(">H", n))
        else:
            hdr.append(0x80 | 127)
            hdr.extend(struct.pack(">Q", n))
        mask = os.urandom(4)
        hdr.extend(mask)
        self.sock.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _frame(self):
        b1, b2 = self._exact(2)
        fin, op, ln, masked = b1 & 0x80, b1 & 0x0F, b2 & 0x7F, b2 & 0x80
        if ln == 126:
            ln = struct.unpack(">H", self._exact(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self._exact(8))[0]
        mk = self._exact(4) if masked else None
        data = self._exact(ln)
        if mk:
            data = bytes(b ^ mk[i % 4] for i, b in enumerate(data))
        if op == 9:                       # ping -> pong
            self._send(data, opcode=10)
            return None
        if op == 8:
            raise RuntimeError("WebSocket 被对端关闭")
        return fin, op, data

    def call(self, method, params=None, timeout=12):
        self._id += 1
        mid = self._id
        self._send(json.dumps({"id": mid, "method": method,
                               "params": params or {}}, ensure_ascii=False))
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            self.sock.settimeout(max(0.3, end - time.time()))
            try:
                f = self._frame()
            except socket.timeout:
                break
            if not f:
                continue
            fin, op, data = f
            if op in (1, 0):             # text / continuation
                buf += data
                if not fin:
                    continue
                try:
                    msg = json.loads(buf.decode("utf-8", "replace"))
                except Exception:
                    buf = b""
                    continue
                buf = b""
                if msg.get("id") == mid:
                    if "error" in msg:
                        raise RuntimeError(f"CDP {method}: {msg['error']}")
                    return msg.get("result", {})
        raise TimeoutError(f"CDP {method} 超时")

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


class Browser:
    def __init__(self, port=PORT):
        global PORT, BASE
        PORT, BASE = port, f"http://127.0.0.1:{port}"
        self.launched = False
        self._active_ws = None      # 固定操作同一个标签页，避免每次动作开一堆新标签

    def available(self):
        try:
            _http_json("/json/version", timeout=1.5)
            return True
        except Exception:
            return False

    def ensure(self):
        """确保有一个可控制的 Chrome 实例；返回 (是否可用, 说明)。"""
        if self.available():
            return True, "已连接受控浏览器"
        if not os.path.exists(CHROME):
            # Safari 或别的机器：退回系统默认 Chrome 路径
            return False, "没有找到 Google Chrome，无法启动浏览器控制"
        # --no-sandbox 是必须的：本进程运行在受限沙箱里，Chrome 子进程
        # 自己初始化 sandbox 会报 "Operation not permitted" 并整体退出。
        args = [CHROME,
                f"--remote-debugging-port={PORT}",
                "--remote-allow-origins=*",
                f"--user-data-dir={PROFILE}",
                "--no-first-run", "--no-default-browser-check",
                "--no-sandbox", "--disable-gpu",
                "--disable-dev-shm-usage", "--disable-background-networking",
                "--window-size=1440,960", "about:blank"]
        try:
            subprocess.Popen(args, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        except Exception as e:
            return False, f"启动浏览器失败: {e}"
        for _ in range(30):
            time.sleep(0.5)
            if self.available():
                self.launched = True
                return True, "已启动受控浏览器窗口"
        return False, "浏览器已启动，但调试端口未就绪（可能被系统拦截）"

    def tabs(self):
        try:
            return [t for t in _http_json("/json/list") if t.get("type") == "page"]
        except Exception:
            return []

    def new_tab(self, url="about:blank"):
        try:
            return _http_json("/json/new?" + urllib.parse.quote(url, safe=":/?=&%"),
                              method="PUT", timeout=5)
        except Exception:
            return {}

    def _ws_url(self, prefer_url=None):
        tabs = self.tabs()
        if prefer_url:
            for t in tabs:
                if prefer_url in (t.get("url") or ""):
                    self._active_ws = t.get("webSocketDebuggerUrl")
                    return self._active_ws
        # 固定标签：用户说「打开浏览器然后搜索」时，后续动作应发生在同一个页面上
        if self._active_ws and any(t.get("webSocketDebuggerUrl") == self._active_ws
                                   for t in tabs):
            return self._active_ws
        chosen = (tabs[0].get("webSocketDebuggerUrl") if tabs
                  else self.new_tab().get("webSocketDebuggerUrl"))
        if chosen:
            self._active_ws = chosen
        return chosen

    def call(self, method, params=None, prefer_url=None, timeout=12):
        ws_url = self._ws_url(prefer_url)
        if not ws_url:
            raise RuntimeError("没有可用的浏览器标签页")
        ws = _WS(ws_url)
        try:
            return ws.call(method, params, timeout=timeout)
        finally:
            ws.close()

    def eval(self, expr, prefer_url=None, timeout=12):
        r = self.call("Runtime.evaluate",
                      {"expression": expr, "returnByValue": True,
                       "awaitPromise": True, "userGesture": True},
                      prefer_url=prefer_url, timeout=timeout)
        res = r.get("result", {})
        if r.get("exceptionDetails"):
            raise RuntimeError(str(r["exceptionDetails"])[:200])
        return res.get("value")

    def navigate(self, url, new=False):
        """默认复用当前标签页（跟真人操作一致）；new=True 才另开标签。"""
        if new:
            tab = self.new_tab(url)
            if tab.get("id"):
                self._active_ws = tab.get("webSocketDebuggerUrl")
                return {"ok": True, "newTab": True}
        return self.call("Page.navigate", {"url": url})

    def state(self, prefer_url=None, limit=1200):
        """读回真实页面：标题、URL、正文、可点元素。"""
        js = r"""(function(){
          const t = document.title || '';
          const u = location.href || '';
          const body = (document.body ? document.body.innerText : '') || '';
          const els = [];
          document.querySelectorAll('a,button,[role=button],input,textarea').forEach(function(e){
            const s = (e.innerText || e.value || e.getAttribute('aria-label') || e.title || '').trim();
            if (s && s.length < 80 && els.length < 60) els.push({tag:e.tagName.toLowerCase(), text:s, href:e.href||''});
          });
          return JSON.stringify({title:t, url:u, text:body.slice(0, 3000), elements:els});
        })()"""
        raw = self.eval(js, prefer_url=prefer_url)
        try:
            return json.loads(raw) if isinstance(raw, str) else (raw or {})
        except Exception:
            return {"title": "", "url": "", "text": str(raw)[:limit], "elements": []}

    def type_text(self, s, prefer_url=None):
        return self.call("Input.insertText", {"text": s}, prefer_url=prefer_url)

    def key(self, name="Enter", prefer_url=None):
        """真实按键（回车 / 空格 / 方向键…）。"""
        vk = {"Enter": 13, "Tab": 9, "Space": 32, "Escape": 27,
              "ArrowDown": 40, "ArrowUp": 38}.get(name, 13)
        base = {"key": name, "code": name, "windowsVirtualKeyCode": vk,
                "nativeVirtualKeyCode": vk}
        self.call("Input.dispatchKeyEvent", dict(base, type="rawKeyDown"), prefer_url=prefer_url)
        if name in ("Enter", "Space", "Tab"):
            self.call("Input.dispatchKeyEvent", dict(base, type="char"), prefer_url=prefer_url)
        self.call("Input.dispatchKeyEvent", dict(base, type="keyUp"), prefer_url=prefer_url)
        return True

    def click(self, text="", index=0, prefer_url=None):
        """按可见文字点击链接/按钮；text 为空时点第 index 个结果链接。"""
        js = r"""(function(txt, idx){
          const want = (txt||'').trim();
          const noise = /^(跳至内容|辅助功能反馈|登录|网页|图片|视频|学术|词典|地图|航班|国内版|国际版|清除本文|设置|反馈|举报|Rewards|更多)$/;
          let list = [];
          document.querySelectorAll('a,button,[role=button],[onclick]').forEach(function(e){
            const s = (e.innerText || e.value || e.getAttribute('aria-label') || e.title || '').trim();
            if (s && s.length < 120 && !noise.test(s)) list.push({e:e, s:s});
          });
          // 没指定文字时，「点第 N 个结果」应命中搜索结果标题，而不是导航链接
          let results = [];
          document.querySelectorAll('h2 a, .b_algo h2 a, li.b_algo h2 a, h3 a').forEach(function(e){
            const s = (e.innerText || '').trim();
            if (s) results.push({e:e, s:s});
          });
          let hit = null, how = '';
          if (want) {
            const exact = list.filter(x => x.s === want);
            const part  = list.filter(x => x.s.indexOf(want) >= 0 || want.indexOf(x.s) >= 0);
            hit = (exact[0] || part[0] || {}).e; how = '文字匹配';
          } else if (results.length) {
            const r = results[Math.min(idx, results.length-1)];
            hit = r.e; how = '第 ' + (idx+1) + ' 条搜索结果';
          } else if (list[idx]) { hit = list[idx].e; how = '第 ' + (idx+1) + ' 个链接'; }
          if (!hit) return JSON.stringify({clicked:false, reason:'没有找到可点元素', count:list.length,
                                          sample:list.slice(0,8).map(x=>x.s)});
          const label = (hit.innerText || hit.getAttribute('aria-label') || '').trim().slice(0,60);
          hit.scrollIntoView({block:'center'});
          // 搜索结果常带 target=_blank；改成当前标签导航，才能接着读回点开后的内容
          if (hit.tagName === 'A') hit.setAttribute('target', '_self');
          hit.click();
          return JSON.stringify({clicked:true, how:how, label:label, href:hit.href||''});
        })(""" + json.dumps(text or "", ensure_ascii=False) + "," + str(int(index)) + ")"
        raw = self.eval(js, prefer_url=prefer_url)
        try:
            return json.loads(raw) if isinstance(raw, str) else (raw or {})
        except Exception:
            return {"clicked": False, "reason": str(raw)[:150]}


_browser = None


def get_browser():
    global _browser
    if _browser is None:
        _browser = Browser()
    return _browser


if __name__ == "__main__":
    b = get_browser()
    ok, msg = b.ensure()
    print("ensure:", ok, msg)
    if ok:
        print("state:", json.dumps(b.state(), ensure_ascii=False)[:600])
