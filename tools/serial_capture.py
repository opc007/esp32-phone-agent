#!/usr/bin/env python3
"""
串口抓日志小工具（ESP32 / ESP32-S31 通用）

用法：
  python3 tools/serial_capture.py                      # 打开串口抓 20 秒
  python3 tools/serial_capture.py --seconds 60         # 抓 60 秒
  python3 tools/serial_capture.py --reset              # 抓之前先复位（走 esptool hard_reset）
  python3 tools/serial_capture.py --delay 15 --send "qprobe" --send "qrotate" --send "qprobe"
                                                 # 等 15 秒再发命令，命令之间隔 3 秒
  python3 tools/serial_capture.py --grep Qibao         # 只打印含关键字的行
  python3 tools/serial_capture.py --out /tmp/s31.log   # 顺便存一份完整日志

注意：
  - macOS 没有 timeout 命令，别用 shell 的 timeout 包一层；
  - RTS/DTR 组合会把芯片拉进下载模式，所以这里显式把两根线都拉低，
    复位交给 esptool --after hard_reset 做；
  - 设备冷启动要十几秒才建好 UI，所以命令要等 --delay 秒之后再发。
"""
import argparse
import os
import subprocess
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-2110"
ESPTOOL = os.path.expanduser("~/.espressif/python_env/idf6.1_py3.13_env/bin/esptool")


def find_esptool():
    if os.path.exists(ESPTOOL):
        return [ESPTOOL]
    return [sys.executable, "-m", "esptool"]


def hard_reset(port):
    """用 esptool 让芯片复位，同时不占用串口。"""
    cmd = find_esptool() + [
        "--chip", "esp32s31", "--port", port,
        "--after", "hard_reset", "chip-id",
    ]
    try:
        subprocess.run(cmd, capture_output=True, timeout=25)
    except Exception as exc:  # 复位失败也要继续抓日志
        print(f"[warn] 复位失败（继续抓日志）: {exc}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--seconds", type=float, default=20, help="总抓取时长")
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--send", action="append", default=[], help="要发的命令，可重复")
    ap.add_argument("--delay", type=float, default=0.5,
                    help="发第一条命令前先等这么久（设备冷启动要十几秒才建好 UI）")
    ap.add_argument("--gap", type=float, default=3.0, help="两条命令之间的间隔")
    ap.add_argument("--grep", default=None)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    import serial

    # 先复位再开串口：芯片一复位就开始打印，抓到完整启动日志
    if args.reset:
        hard_reset(args.port)
        time.sleep(0.3)

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    ser.dtr = False
    ser.rts = False

    buf = []
    start = time.time()
    deadline = start + args.seconds

    # 命令发送计划：delay 之后发第一条，之后每隔 gap 发一条
    pending = list(args.send)
    next_send_at = start + args.delay

    while time.time() < deadline:
        now = time.time()
        if pending and now >= next_send_at:
            cmd = pending.pop(0)
            ser.write((cmd + "\r\n").encode("utf-8"))
            ser.flush()
            print(f"\n>>> 发送命令: {cmd}\n", flush=True)
            next_send_at = now + args.gap

        data = ser.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "ignore")
        buf.append(text)
        if args.grep:
            for line in text.splitlines():
                if args.grep in line:
                    print(line, flush=True)
        else:
            sys.stdout.write(text)
            sys.stdout.flush()

    ser.close()
    full = "".join(buf)
    if args.out:
        with open(args.out, "w") as f:
            f.write(full)
        print(f"\n[save] {args.out} ({len(full)} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
