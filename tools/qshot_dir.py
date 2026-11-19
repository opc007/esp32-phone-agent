#!/usr/bin/env python3
"""
按指定方向抓屏幕快照 + 跑旋转标记诊断。

流程：等设备就绪 -> 发 qrot <角度> -> 等界面重建完 -> 发 qshot 抓图
      -> 可选发 qdiag 看洋红标记落在哪个角。

用法：
  python3 tools/qshot_dir.py --dir 90  --out /tmp/q90.jpg  --diag
  python3 tools/qshot_dir.py --dir 270 --out /tmp/q270.jpg

说明：
  * 打开串口时 dtr/rts 都拉低，不会复位设备（qshot.py 同款做法），
    所以不会打断正在跑的对话。
  * 换方向时横屏<->竖屏要重建整套界面（人物图要按新尺寸重新解码，
    竖屏那套约 450x800，约 400ms），所以抓图前多等一会儿。
"""
import argparse
import base64
import re
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-2110"
SHOT_RE = re.compile(r"\[SHOT\]\s+([A-Za-z0-9+/=]+)")
READY_RE = re.compile(r"\[CMD\] < \{")


def collect_base64(full):
    lines = []
    for line in full.splitlines():
        m = SHOT_RE.search(line)
        if not m:
            continue
        piece = m.group(1)
        if piece == "BEGIN" or piece == "END":
            continue
        lines.append(piece)
    if not lines:
        return None
    try:
        return base64.b64decode("".join(lines))
    except Exception as exc:
        print(f"[shot] base64 解码失败: {exc}", file=sys.stderr)
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--dir", type=int, required=True,
                    help="目标角度：0 / 90 / 180 / 270")
    ap.add_argument("--out", default=None, help="快照保存路径")
    ap.add_argument("--diag", action="store_true", help="同时跑 qdiag 标记测试")
    ap.add_argument("--timeout", type=float, default=70.0)
    ap.add_argument("--settle", type=float, default=4.0, help="换方向后等待界面重建的秒数")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    import serial

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    ser.dtr = False
    ser.rts = False

    buf = []
    ready = False
    deadline = time.time() + args.timeout
    next_ping = time.time() + 8.0

    # 阶段一：等设备就绪
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
        if args.verbose:
            sys.stdout.write(text)
            sys.stdout.flush()
        if READY_RE.search(text):
            ready = True

    if not ready:
        ser.close()
        print("[shot] ✗ 设备没就绪")
        return 1

    # 阶段二：切方向
    ser.write(f"qrot {args.dir}\r\n".encode())
    ser.flush()
    t_end = time.time() + args.settle
    while time.time() < t_end:
        data = ser.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "ignore")
        buf.append(text)
        if args.verbose:
            sys.stdout.write(text)
            sys.stdout.flush()

    # 阶段三：抓快照
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

    # 阶段四（可选）：标记诊断
    if args.diag:
        ser.write(b"qdiag\r\n")
        ser.flush()
        diag_end = time.time() + 5.0
        while time.time() < diag_end:
            data = ser.read(4096)
            if not data:
                continue
            text = data.decode("utf-8", "ignore")
            buf.append(text)
            if "[标记] 标记在" in text:
                break

    ser.close()
    full = "".join(buf)

    if args.out:
        jpeg = collect_base64(full)
        if jpeg is None:
            print("[shot] ✗ 没抓到快照")
            return 1
        with open(args.out, "wb") as f:
            f.write(jpeg)
        print(f"[shot] ✓ {args.dir}° 快照已保存: {args.out} ({len(jpeg)} bytes)")

    # 把方向/标记相关的关键日志打出来
    for line in full.splitlines():
        if any(k in line for k in ("[标记]", "屏幕方向", "方向切换", "应用屏幕变换",
                                   "布局根容器", "人物图解码")):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
