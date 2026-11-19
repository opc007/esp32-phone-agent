#!/usr/bin/env python3
"""
抓一张「指定方向」的屏幕快照，用于肉眼/几何比对。

流程：等设备就绪 -> 发 qrotate（切方向） -> 等一拍 -> 发 qshot -> 还原 JPEG。

用法：
  python3 tools/qshot_at.py --out /tmp/rot180.jpg --rotate 1
  python3 tools/qshot_at.py --out /tmp/rot0.jpg   --rotate 0
"""
import argparse
import base64
import re
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-2110"
SHOT_RE = re.compile(r"\[SHOT\]\s+([A-Za-z0-9+/=]+)")
READY_RE = re.compile(r"\[CMD\] < \{")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--out", default="/tmp/qshot.jpg")
    ap.add_argument("--rotate", type=int, default=None,
                    help="先发几次 qrotate（每发一次切一个方向）")
    ap.add_argument("--timeout", type=float, default=70.0)
    args = ap.parse_args()

    import serial

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    ser.dtr = False
    ser.rts = False

    buf = []
    ready = False
    deadline = time.time() + args.timeout
    next_ping = time.time() + 8.0
    while time.time() < deadline and not ready:
        now = time.time()
        if now >= next_ping:
            ser.write(b"qstatus\r\n")
            ser.flush()
            next_ping = now + 2.0
        data = ser.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "ignore")
        buf.append(text)
        if READY_RE.search(text):
            ready = True

    if not ready:
        ser.close()
        print("[shot] ✗ 设备没就绪")
        return 1
    print("[shot] 设备就绪")

    # 切方向
    if args.rotate:
        for i in range(args.rotate):
            ser.write(b"qrotate\r\n")
            ser.flush()
            time.sleep(1.5)
        time.sleep(2.0)
        buf.append(ser.read(4096).decode("utf-8", "ignore"))

    # 抓快照
    ser.write(b"qshot\r\n")
    ser.flush()
    shot_deadline = time.time() + 25.0
    while time.time() < shot_deadline:
        data = ser.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "ignore")
        buf.append(text)
        if "[SHOT] END" in text:
            break
    ser.close()

    full = "".join(buf)
    b64_lines = []
    for line in full.splitlines():
        m = SHOT_RE.search(line)
        if not m:
            continue
        piece = m.group(1)
        if piece in ("BEGIN", "END"):
            continue
        b64_lines.append(piece)
    if not b64_lines:
        print("[shot] ✗ 没抓到快照数据")
        return 1
    data = base64.b64decode("".join(b64_lines))
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"[shot] ✓ {args.out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
