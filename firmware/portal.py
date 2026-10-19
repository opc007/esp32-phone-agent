"""Mobilerun Portal HTTP 客户端（MicroPython）。

Portal 在手机上监听 0.0.0.0:8080，所有请求带 Bearer token。
返回体形如 {"status":"success","result": <对象或 JSON 字符串>}。
"""
import json
import socket
import time


class PortalError(Exception):
    pass


class Portal:
    def __init__(self, host, port=8080, token=None, timeout=10):
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout

    # ── HTTP 底层 ──────────────────────────────────────────
    def _recv_all(self, sock):
        buf = bytearray()
        while True:
            try:
                chunk = sock.recv(1024)
            except OSError:
                break
            if not chunk:
                break
            buf.extend(chunk)
            if len(buf) > 512 * 1024:  # 防御：异常大响应
                break
        return bytes(buf)

    def _dechunk(self, data):
        out = bytearray()
        while data:
            nl = data.find(b"\r\n")
            if nl < 0:
                break
            try:
                size = int(data[:nl].split(b";")[0], 16)
            except ValueError:
                break
            if size == 0:
                break
            start = nl + 2
            out.extend(data[start:start + size])
            data = data[start + size + 2:]
        return bytes(out)

    def request(self, method, path, body=None, timeout=None):
        payload = b""
        if body is not None:
            payload = json.dumps(body).encode()

        head = "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n" % (
            method, path, self.host, self.port)
        if self.token:
            head += "Authorization: Bearer %s\r\n" % self.token
        if payload:
            head += "Content-Type: application/json\r\n"
            head += "Content-Length: %d\r\n" % len(payload)

        raw = b""
        last_err = None
        for attempt in range(2):
            sock = socket.socket()
            sock.settimeout(timeout or self.timeout)
            try:
                sock.connect((self.host, self.port))
                sock.sendall(head.encode() + b"\r\n" + payload)
                raw = self._recv_all(sock)
            except OSError as exc:
                last_err = exc
                raw = b""
            finally:
                try:
                    sock.close()
                except OSError:
                    pass
            if raw:
                break
            time.sleep(0.4)
        if not raw and last_err is not None:
            raise PortalError("transport error: %s" % last_err)

        sep = raw.find(b"\r\n\r\n")
        if sep < 0:
            raise PortalError("bad HTTP response")
        header = raw[:sep].decode("utf-8", "replace")
        data = raw[sep + 4:]

        status_line = header.split("\r\n")[0] if header else ""
        parts = status_line.split(" ")
        status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
        if "transfer-encoding: chunked" in header.lower():
            data = self._dechunk(data)
        if status in (401, 403):
            raise PortalError("auth failed (token rotated?)")
        if status != 200:
            raise PortalError("HTTP %d: %s" % (status, header[:120]))

        if not data:
            return None
        if isinstance(data, (bytes, bytearray)):
            data = data.decode("utf-8", "replace")
        try:
            obj = json.loads(data)
        except ValueError:
            return data
        return self._unwrap(obj)

    @staticmethod
    def _unwrap(obj):
        if isinstance(obj, dict):
            if obj.get("status") == "error":
                raise PortalError(str(obj.get("message") or obj)[:200])
            if "result" in obj:
                obj = obj["result"]
            elif "data" in obj:
                obj = obj["data"]
            if isinstance(obj, str):
                try:
                    return json.loads(obj)
                except ValueError:
                    return obj
        return obj

    # ── 高层 API ───────────────────────────────────────────
    def ping(self):
        return self.request("GET", "/ping")

    def version(self):
        return self.request("GET", "/version")

    def state(self):
        return self.request("GET", "/state")

    def tap(self, x, y):
        return self.request("POST", "/tap", {"x": int(x), "y": int(y)})

    def swipe(self, x1, y1, x2, y2, duration=600):
        return self.request("POST", "/swipe", {
            "startX": int(x1), "startY": int(y1),
            "endX": int(x2), "endY": int(y2),
            "duration": float(duration),
        })

    def key(self, key_code):
        return self.request("POST", "/keyboard/key", {"key_code": int(key_code)})

    def input_text(self, text, clear=True):
        import base64
        encoded = base64.b64encode(text.encode()).decode()
        return self.request("POST", "/keyboard/input",
                            {"base64_text": encoded, "clear": clear})

    def start_app(self, package):
        return self.request("POST", "/app", {"package": package})

    def stop_app(self, package):
        return self.request("POST", "/app/stop", {"package": package})


# Android KeyEvent
KEY_HOME = 3
KEY_BACK = 4
KEY_ENTER = 66
KEY_DEL = 67
