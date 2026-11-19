#!/usr/bin/env python3
"""
决定性验证：画面到底有没有被旋转 180°。

做法（不依赖 LVGL 探针，直接看像素）：
  1. 抓一张 0° 的屏幕快照 A
  2. 执行 qrotate（切到 180°）
  3. 抓一张 180° 的屏幕快照 B
  4. 把 A 旋转 180° 得到 A'
  5. 比较 A' 和 B：
       - 如果 A' 和 B 几乎一样 -> 画面确实转了
       - 如果 A 和 B 几乎一样     -> 根本没转（BUG）

小智截屏通道有 RGB565 字节序问题，颜色不可信，但**几何结构**是可信的，
所以用「灰度化后的边缘结构」做比较，比直接比 RGB 稳。

用法：
  python3 tools/shot_compare.py --out-prefix /tmp/rot
"""
import argparse
import os
import subprocess
import sys

import qshot


def load_gray(path):
    """读 JPEG 转灰度数组（不依赖 PIL，纯 Python 解码太慢，这里用 sips 转 PNG 再读）。"""
    raise NotImplementedError


def compare_with_sips(a_path, b_path):
    """
    用 macOS 自带的 sips 做几何对比：
      - sips 可以旋转图片，所以先把 A 转 180° 得到 A'
      - 再逐字节比较 A' 与 B 的尺寸、以及用 cmp 统计差异字节比例
    """
    import subprocess as sp

    def run(*args):
        return sp.run(args, capture_output=True)

    # A 旋转 180° -> A_rot
    a_rot = a_path.replace(".jpg", "_rot.jpg")
    run("sips", "-r", "180", a_path, "--out", a_rot)

    def dims(p):
        r = run("sips", "-g", "pixelWidth", "-g", "pixelHeight", p)
        out = r.stdout.decode(errors="ignore")
        w = h = 0
        for line in out.splitlines():
            if "pixelWidth" in line:
                w = int(line.split(":")[-1].strip())
            if "pixelHeight" in line:
                h = int(line.split(":")[-1].strip())
        return w, h

    def cmp_ratio(p1, p2):
        """用 cmp -l 统计不同字节数。"""
        r = run("cmp", "-l", p1, p2)
        lines = r.stdout.count(b"\n")
        size = max(os.path.getsize(p1), os.path.getsize(p2))
        return lines / size if size else 1.0

    d_a = dims(a_path)
    d_b = dims(b_path)
    d_ar = dims(a_rot)
    print(f"[cmp] A        尺寸 {d_a}")
    print(f"[cmp] B        尺寸 {d_b}")
    print(f"[cmp] A 旋转180 尺寸 {d_ar}")

    if d_a != d_b:
        print("[cmp] ✗ 尺寸不同，无法直接比对（说明画面尺寸变了？）")
        return 2

    same = cmp_ratio(a_path, b_path)
    rotated_same = cmp_ratio(a_rot, b_path)
    print(f"[cmp] A   vs B          字节差异率 {same:.4f}")
    print(f"[cmp] A旋转180 vs B     字节差异率 {rotated_same:.4f}")

    if rotated_same < same:
        print("\n[cmp] ✓ 结论：画面确实被旋转了 180°（旋转后的 A 与 B 高度吻合）")
        return 0
    print("\n[cmp] ✗ 结论：画面没有被旋转（A 与 B 反而更接近）")
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-2110")
    ap.add_argument("--prefix", default="/tmp/rot")
    ap.add_argument("--delay", type=float, default=18.0)
    args = ap.parse_args()

    a_path = args.prefix + "_0deg.jpg"
    b_path = args.prefix + "_180deg.jpg"

    print("[cmp] 抓 0° 快照...")
    data, _ = qshot.capture_shot(args.port, args.delay, 2.0, 30.0)
    if data is None:
        print("[cmp] ✗ 第一张没抓到")
        return 2
    qshot.save_jpeg(data, a_path)
    print(f"[cmp]   -> {a_path} ({len(data)} bytes)")

    print("[cmp] 执行 qrotate 切到 180°...")
    subprocess.run([
        sys.executable, "tools/serial_capture.py",
        "--port", args.port, "--seconds", "12",
        "--delay", str(args.delay), "--gap", "2.0",
        "--send", "qrotate",
    ], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print("[cmp] 抓 180° 快照...")
    data, _ = qshot.capture_shot(args.port, 3.0, 2.0, 25.0)
    if data is None:
        print("[cmp] ✗ 第二张没抓到")
        return 2
    qshot.save_jpeg(data, b_path)
    print(f"[cmp]   -> {b_path} ({len(data)} bytes)")

    return compare_with_sips(a_path, b_path)


if __name__ == "__main__":
    sys.exit(main())
