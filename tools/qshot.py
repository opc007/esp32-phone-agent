#!/usr/bin/env python3
"""
抓 qshot 屏幕快照并还原成图片，用来肉眼核对画面。

关键点：
  1. 打开串口会复位设备，设备要十几秒才建好 UI。太早发 qshot 会得到
     「显示还没初始化」。所以这里**轮询 qstatus 直到有应答**再抓快照，
     比固定 sleep 稳。
  2. 设备上的 qshot 是自写 base64 分段输出，每行以 "[SHOT] " 开头，
     拼的时候必须用 carry 缓冲，否则跨块的那一行会被丢掉（约 15 行）。
  3. 小智的截屏通道有 RGB565 字节序问题，颜色会偏，但**几何位置是对的**，
     够用来判断画面有没有翻转。

用法：
  python3 tools/qshot.py --out /tmp/shot.jpg
"""
import argparse
import base64
import os
import re
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-2110"
SHOT_RE = re.compile(r"\[SHOT\]\s+([A-Za-z0-9+/=]+)")
READY_RE = re.compile(r"\[CMD\] < \{")


def capture_shot(port, ready_timeout=60.0, shot_timeout=25.0, verbose=False):
    """
    打开串口 -> 等设备就绪 -> 发 qshot -> 收集 base64 -> 返回 JPEG 字节。
    """
    import serial

    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.dtr = False
    ser.rts = False

    buf = []
    ready = False
    deadline = time.time() + ready_timeout
    next_ping = time.time() + 8.0  # 先等 8 秒让设备启动

    # 阶段一：等就绪
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
        if verbose:
            sys.stdout.write(text)
            sys.stdout.flush()
        if READY_RE.search(text):
            ready = True

    if not ready:
        ser.close()
        return None, "".join(buf)

    # 阶段二：抓快照
    ser.write(b"qshot\r\n")
    ser.flush()
    shot_deadline = time.time() + shot_timeout
    while time.time() < shot_deadline:
        data = ser.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "ignore")
        buf.append(text)
        if verbose:
            sys.stdout.write(text)
            sys.stdout.flush()
        if "[SHOT] END" in text:
            break

    ser.close()
    full = "".join(buf)

    # 关键：base64 分段拼接。
    # 设备端每行固定输出 240 个字符（最后一行是余数），行首有 "[SHOT] " 前缀。
    # 注意 "[SHOT] BEGIN <长度>" 和 "[SHOT] END" 是标记行，不是数据，必须排除，
    # 否则会把 "BEGIN" 当成 base64 片段混进去，解码直接报错。
    b64_lines = []
    for line in full.splitlines():
        m = SHOT_RE.search(line)
        if not m:
            continue
        piece = m.group(1)
        if piece == "BEGIN" or piece == "END":
            continue
        b64_lines.append(piece)

    if not b64_lines:
        return None, full
    try:
        data = base64.b64decode("".join(b64_lines))
    except Exception as exc:
        print(f"[shot] base64 解码失败: {exc}", file=sys.stderr)
        return None, full
    return data, full


def save_jpeg(data, path):
    with open(path, "wb") as f:
        f.write(data)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--out", default="/tmp/qshot.jpg")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    data, _ = capture_shot(args.port, args.timeout, 25.0, args.verbose)
    if data is None:
        print(f"[shot] ✗ 没抓到快照")
        return 1
    save_jpeg(data, args.out)
    print(f"[shot] ✓ 快照已保存: {args.out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
