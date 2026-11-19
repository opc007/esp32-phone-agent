#!/usr/bin/env python3
"""
给设备发一条串口控制台命令，并把回复打出来。

用法：
  python3 tools/qcmd.py qstatus
  python3 tools/qcmd.py qrot 90
  python3 tools/qcmd.py help --timeout 60

背景：打开串口本身会拉动 DTR/RTS，把芯片复位一次，所以这里沿用
qshot/qcam 那套做法 —— 打开后先把 dtr/rts 置低，然后每 2 秒 ping 一次
qstatus，直到设备控制台就绪（出现 "[CMD] < {" 标记）再发真正的命令。

注意：串口是独占的，同一时刻只能跑一个抓取脚本。
"""
import argparse
import re
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-2110"
READY_RE = re.compile(r"\[CMD\] < \{")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", nargs="+", help="要发的控制台命令，例如 qstatus / qrot 90")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=float, default=60.0, help="等设备就绪的最长秒数")
    ap.add_argument("--settle", type=float, default=2.5, help="命令发出去后等回复的秒数")
    ap.add_argument("--verbose", action="store_true", help="把设备所有串口输出都打出来")
    args = ap.parse_args()

    import serial

    cmd = " ".join(args.command).strip()

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    ser.dtr = False
    ser.rts = False

    buf = []
    ready = False
    deadline = time.time() + args.timeout
    next_ping = time.time() + 3.0
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
        print("[qcmd] ✗ 设备没就绪（控制台没起来）")
        return 1

    # 清掉就绪探测阶段的输出，只留真正命令的回复
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    ser.flush()

    end = time.time() + args.settle
    reply = []
    while time.time() < end:
        data = ser.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "ignore")
        reply.append(text)
        if args.verbose:
            sys.stdout.write(text)
            sys.stdout.flush()

    ser.close()
    out = "".join(reply).strip()
    print(out if out else "[qcmd] （无回复）")
    return 0 if out else 2


if __name__ == "__main__":
    sys.exit(main())
