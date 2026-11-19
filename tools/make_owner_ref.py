#!/usr/bin/env python3
"""
把主人的一张照片，按人脸框裁成两帧「设备同款」4:3 JPG：
  - enroll 帧：用来开机录入主人特征（脸占比略大）
  - verify 帧：取景略有差异，用来做离线自检（同一人不同取景应高相似度）
两帧都会走设备端 jpeg_to_image_fit 解码，和摄像头实时帧完全同一条图像管线，
因此提取出的 512 维特征与实时刷脸可比。

依赖：macOS sips（JPG<->BMP）+ Python 标准库，无需 PIL/numpy。
用法：python3 tools/make_owner_ref.py <照片> <输出.h>
"""
import struct
import subprocess
import sys
import os
import tempfile


def to_bmp(src: str, bmp: str):
    subprocess.run(["sips", "-s", "format", "bmp", src, "--out", bmp],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_bmp(path: str):
    d = open(path, "rb").read()
    assert d[:2] == b"BM"
    off = struct.unpack_from("<I", d, 10)[0]
    w = struct.unpack_from("<i", d, 18)[0]
    h_raw = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 24, f"need 24bpp, got {bpp}"
    h = abs(h_raw)
    row = ((w * 3 + 3) // 4) * 4
    bottom_up = h_raw > 0
    # 统一成自上而下的 (r,g,b) 行
    rows = []
    for y in range(h):
        src_row = (h - 1 - y) if bottom_up else y
        base = off + src_row * row
        line = []
        for x in range(w):
            b, g, r = d[base + x * 3: base + x * 3 + 3]
            line.append((r, g, b))
        rows.append(line)
    return w, h, rows


def crop_resize(rows, cx, cy, cw, ch, out_w, out_h):
    """从 (cx,cy) 为中心裁 cw x ch(4:3)，最近邻缩放到 out_w x out_h。"""
    H = len(rows)
    W = len(rows[0])
    x0 = cx - cw // 2
    y0 = cy - ch // 2
    out = []
    for y in range(out_h):
        sy = y0 + y * ch // out_h
        sy = max(0, min(H - 1, sy))
        line = []
        for x in range(out_w):
            sx = x0 + x * cw // out_w
            sx = max(0, min(W - 1, sx))
            line.append(rows[sy][sx])
        out.append(line)
    return out


def write_bmp(path: str, rows):
    h = len(rows)
    w = len(rows[0])
    row = ((w * 3 + 3) // 4) * 4
    data = bytearray()
    for y in range(h):
        line = bytearray()
        for (r, g, b) in rows[y]:
            line += bytes((b, g, r))
        line += b"\x00" * (row - len(line))
        data += line
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    struct.pack_into("<I", hdr, 2, 54 + len(data))
    struct.pack_into("<I", hdr, 10, 54)
    struct.pack_into("<I", hdr, 14, 40)
    struct.pack_into("<i", hdr, 18, w)
    struct.pack_into("<i", hdr, 22, -h)  # top-down
    struct.pack_into("<H", hdr, 26, 1)
    struct.pack_into("<H", hdr, 28, 24)
    open(path, "wb").write(hdr + data)


def bmp_to_jpg(bmp: str, jpg: str, quality: int):
    subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions",
                    str(quality), bmp, "--out", jpg],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def emit_array(f, jpg: str, name: str):
    data = open(jpg, "rb").read()
    f.write(f"static const unsigned char {name}[] = {{\n")
    for i in range(0, len(data), 16):
        f.write("    " + "".join(f"0x{b:02X}," for b in data[i:i + 16]) + "\n")
    f.write("};\n\n")
    return len(data)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    src, out_h = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as td:
        bmp = os.path.join(td, "src.bmp")
        to_bmp(src, bmp)
        W, H, rows = read_bmp(bmp)
        print(f"source {W}x{H}")

        # macOS Vision 检测到的人脸框（针对这张照片）：x=394 y=736 w=530 h=530
        # 按比例换算，兼容其它尺寸输入
        fx, fy, fw, fh = 394 / 1279 * W, 736 / 1706 * H, 530 / 1279 * W, 530 / 1706 * H
        cx, cy = fx + fw / 2, fy + fh / 2
        print(f"face center=({cx:.0f},{cy:.0f}) size=({fw:.0f}x{fh:.0f})")

        # 4:3 取景框。enroll 让脸占约 62% 高；verify 取景收紧/偏移，模拟不同距离角度
        # 设备端 jpeg_to_image_fit 解码到 320x240，源图给 320x240 即可，JPG 才够小
        ow, oh = 320, 240
        def frame(scale, dx, dy):
            ch = fh / scale
            cw = ch * 4 / 3
            return crop_resize(rows, int(cx + dx), int(cy + dy), int(cw), int(ch), ow, oh)

        e = frame(0.62, 0, -10)
        v = frame(0.50, 25, 15)
        eb = os.path.join(td, "e.bmp"); vb = os.path.join(td, "v.bmp")
        ej = os.path.join(td, "e.jpg"); vj = os.path.join(td, "v.jpg")
        write_bmp(eb, e); write_bmp(vb, v)
        bmp_to_jpg(eb, ej, 82); bmp_to_jpg(vb, vj, 82)

        with open(out_h, "w") as f:
            f.write("// 由 tools/make_owner_ref.py 自动生成，勿手工编辑\n")
            f.write("#pragma once\n\n#include <stddef.h>\n\n")
            f.write("// 岗哥本人照片裁出的两帧 640x480 JPG（设备端解码到 320x240 后提特征）\n")
            n1 = emit_array(f, ej, "qibao_owner_enroll_jpg")
            n2 = emit_array(f, vj, "qibao_owner_verify_jpg")
        print(f"OK: {out_h} enroll={n1}B verify={n2}B ({ow}x{oh} each)")


if __name__ == "__main__":
    main()
