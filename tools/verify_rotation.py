#!/usr/bin/env python3
"""
严格验证「画面是否真的翻转了 180°」。

思路：探针（qprobe）会对当前送显缓冲做两种采样 ——
  正序：按 (x, y) 正着取
  旋转序：按 (w-1-x, h-1-y) 取
如果画面真的被旋转了 180°，那么「现在的正序」应该等于「旋转前的旋转序」。

所以判据是：
  1. 静止不动时，连续两次 qprobe 的校验和必须完全一样（证明探针稳定、可复现）；
  2. 执行 qrotate 后，正序/旋转序必须互换（证明内容真的被转了）；
  3. 再转一次，必须换回原来的值（证明是可逆的，不是随机噪声）。

用法：
  python3 tools/verify_rotation.py --out /tmp/rot.log
"""
import argparse
import re
import subprocess
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-2110"
PROBE_RE = re.compile(r"正序=([0-9a-f]+)\s+旋转序=([0-9a-f]+)")


def run_capture(port, sends, delay, gap, seconds, out):
    cmd = [
        sys.executable, "tools/serial_capture.py",
        "--port", port,
        "--seconds", str(seconds),
        "--delay", str(delay),
        "--gap", str(gap),
        "--out", out,
    ]
    for s in sends:
        cmd += ["--send", s]
    subprocess.run(cmd, check=False)


def parse_probes(path):
    probes = []
    with open(path, "r", errors="ignore") as f:
        for line in f:
            m = PROBE_RE.search(line)
            if m:
                probes.append((m.group(1), m.group(2)))
    return probes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--delay", type=float, default=18.0, help="开机后等多久开始发命令")
    ap.add_argument("--gap", type=float, default=2.5, help="命令间隔")
    ap.add_argument("--seconds", type=float, default=45.0)
    ap.add_argument("--out", default="/tmp/rot_verify.log")
    args = ap.parse_args()

    # 命令序列：探测 -> 探测（静止对照）-> 旋转 -> 探测 -> 旋转 -> 探测
    sends = ["qprobe", "qprobe", "qrotate", "qprobe", "qrotate", "qprobe"]
    print(f"[verify] 发命令序列: {' -> '.join(sends)}")
    run_capture(args.port, sends, args.delay, args.gap, args.seconds, args.out)

    probes = parse_probes(args.out)
    print(f"\n[verify] 共抓到 {len(probes)} 次探针采样")
    for i, (fwd, rev) in enumerate(probes):
        print(f"         #{i}: 正序={fwd}  旋转序={rev}")

    ok = True
    if len(probes) < 6:
        print("\n[verify] ✗ 探针采样不足 6 次，无法判定（设备可能还没启动完）")
        return 2

    a, b = probes[0], probes[1]
    c = probes[2]
    d = probes[3]
    e = probes[4]

    print("\n[verify] ---- 判定 ----")
    if a == b:
        print("[verify] ✓ 静止时两次采样完全一致 -> 探针稳定，可复现")
    else:
        print("[verify] ✗ 静止时两次采样不一致 -> 探针不稳定，结果不可信")
        ok = False

    if c[0] == a[1] and c[1] == a[0]:
        print("[verify] ✓ 旋转后正序/旋转序互换 -> 内容确实被转了 180°")
    else:
        print(f"[verify] ✗ 旋转后没有互换（期望 {a[1]}/{a[0]}，实际 {c[0]}/{c[1]}）")
        ok = False

    if e[0] == a[0] and e[1] == a[1]:
        print("[verify] ✓ 再转一次回到原值 -> 翻转可逆")
    else:
        print(f"[verify] ✗ 转回后对不上原值（期望 {a[0]}/{a[1]}，实际 {e[0]}/{e[1]}）")
        ok = False

    print("\n[verify] 结论: " + ("全部通过 ✓" if ok else "存在失败项 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
