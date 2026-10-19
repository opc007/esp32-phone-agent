"""DeepSeek Chat Completions 客户端（MicroPython，HTTPS）。"""
import json
import socket
import ssl
import time


class LLMError(Exception):
    pass


class DeepSeek:
    HOST = "api.deepseek.com"

    def __init__(self, api_key, model="deepseek-flash", timeout=75):
        self.api_key = api_key
        self.model = model
        self.timeout = timeout

    def _read_all(self, sock):
        buf = bytearray()
        while True:
            try:
                chunk = sock.recv(1024)
            except OSError:
                break
            if not chunk:
                break
            buf.extend(chunk)
            if len(buf) > 256 * 1024:
                break
        return bytes(buf)

    @staticmethod
    def _dechunk(data):
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

    def chat(self, messages, max_tokens=8000, temperature=0.0):
        body = {
            "model": self.model,
            "messages": messages,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": False,
        }
        payload = json.dumps(body).encode()

        addr = socket.getaddrinfo(self.HOST, 443)[0][-1]
        raw = b""
        last_err = None
        for _attempt in range(2):
            sock = socket.socket()
            sock.settimeout(self.timeout)
            try:
                sock.connect(addr)
                sock = ssl.wrap_socket(sock, server_hostname=self.HOST)
                head = (
                    "POST /chat/completions HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Authorization: Bearer %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n" % (self.HOST, self.api_key, len(payload))
                )
                sock.sendall(head.encode() + payload)
                raw = self._read_all(sock)
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
            time.sleep(0.5)
        if not raw and last_err is not None:
            raise LLMError("transport error: %s" % last_err)

        sep = raw.find(b"\r\n\r\n")
        if sep < 0:
            raise LLMError("bad HTTPS response")
        header = raw[:sep].decode("utf-8", "replace")
        data = raw[sep + 4:]
        line = header.split("\r\n")[0]
        parts = line.split(" ")
        status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
        if "transfer-encoding: chunked" in header.lower():
            data = self._dechunk(data)
        if status != 200:
            raise LLMError("DeepSeek HTTP %d: %s" % (status, data[:200]))
        if isinstance(data, (bytes, bytearray)):
            data = data.decode("utf-8", "replace")
        try:
            obj = json.loads(data)
            choice = obj["choices"][0]
            content = choice["message"].get("content")
            if not content:
                raise LLMError("empty content (finish=%s, reasoning=%d)" % (
                    choice.get("finish_reason"),
                    len(choice["message"].get("reasoning_content") or "")))
            return content
        except (ValueError, KeyError, IndexError) as exc:
            raise LLMError("bad DeepSeek payload: %s" % (exc,))
