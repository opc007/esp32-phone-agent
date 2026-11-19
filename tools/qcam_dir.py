#!/usr/bin/env python3
"""
抓一帧摄像头画面（设备端已按当前屏幕方向去旋转），还原成图片。

用法：
  python3 tools/qcam_dir.py --dir 0   --out /tmp/cam0.jpg
  python3 tools/qcam_dir.py --dir 90  --out /tmp/cam90.jpg

对比不同角度抓到的图，就能看出摄像头画面是不是跟着屏幕方向一起转了：
  0°  -> 320x240（横）
  90° -> 240x320（竖，内容顺时针转了 90°）
  180°-> 320x240（倒转）
  270°-> 240x320（竖，内容逆时针转了 90°）
"""
import argparse
import base64
import re
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-2110"
# base64 数据行用的是 [SHOT] 前缀 —— 设备端的 PrintBase64() 是屏幕快照和
# 摄像头抓帧共用的，它固定打 [SHOT]（不是 [CAM]）。所以这里必须匹配 [SHOT]。
SHOT_RE = re.compile(r"\[SHOT\]\s+([A-Za-z0-9+/=]+)")
READY_RE = re.compile(r"\[CMD\] < \{")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--dir", type=int, default=None,
                    help="先切到这个角度再抓（0/90/180/270）")
    ap.add_argument("--out", default="/tmp/qcam.jpg")
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    import serial

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    ser.dtr = False
    ser.rts = False

    buf = []
    ready = False
    deadline = time.time() + args.timeout
    next_ping = time.time() + 5.0
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
        print("[cam] ✗ 设备没就绪")
        return 1

    if args.dir is not None:
        ser.write(f"qrot {args.dir}\r\n".encode())
        ser.flush()
        t_end = time.time() + 4.0
        while time.time() < t_end:
            data = ser.read(4096)
            if not data:
                continue
            text = data.decode("utf-8", "ignore")
            buf.append(text)
            if args.verbose:
                sys.stdout.write(text)
                sys.stdout.flush()

    # 发 qcam。
    # 【为什么要重发】设备刚开机那几秒日志刷得极多（WiFi + MCP 握手），
    # 串口控制台任务如果正卡在某个耗时操作（比如切方向时整套 UI 重建），
    # UART 接收缓冲会被日志冲掉，命令就丢了 —— 表现是「脚本说没抓到帧，
    # 但设备根本没收到这条命令」。
    # 所以：发出去之后盯着设备的回显 "[CMD] > qcam"，
    # 几秒内没看到就重发，最多 3 次。
    end = time.time() + args.timeout
    sent = 0
    last_send = 0.0
    echoed = False
    while time.time() < end:
        now = time.time()
        if not echoed and sent < 3 and now - last_send > 6.0:
            ser.write(b"qcam\r\n")
            ser.flush()
            sent += 1
            last_send = now
        data = ser.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "ignore")
        buf.append(text)
        if args.verbose:
            sys.stdout.write(text)
            sys.stdout.flush()
        if "[CMD] > qcam" in text:
            echoed = True
        if "[CAM] END" in text:
            break

    ser.close()
    full = "".join(buf)

    pieces = []
    for line in full.splitlines():
        m = SHOT_RE.search(line)
        if not m:
            continue
        p = m.group(1)
        if p in ("BEGIN", "END"):
            continue
        pieces.append(p)
    if not pieces:
        print("[cam] ✗ 没抓到摄像头帧")
        return 1
    try:
        jpeg = base64.b64decode("".join(pieces))
    except Exception as exc:
        print(f"[cam] base64 解码失败: {exc}")
        return 1
    with open(args.out, "wb") as f:
        f.write(jpeg)
    print(f"[cam] ✓ 已保存: {args.out} ({len(jpeg)} bytes)")
    for line in full.splitlines():
        if "de-rotated" in line or "Captured JPEG" in line:
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
