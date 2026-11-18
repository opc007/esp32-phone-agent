#!/usr/bin/env python3
"""
把 JPG 原文件字节转成 C 数组头文件（不做解码），供固件运行时用 jpeg_to_image_fit 解码到 PSRAM。
相比把图片预解码成 RGB565 数组，JPG 体积小得多（几十 KB vs 几百 KB），适合 flash 紧张的板子。

用法：
  python3 tools/jpg2c.py <输入.jpg> <输出.h> <数组名>
例：
  python3 tools/jpg2c.py /tmp/qb_frame0.jpg qibao_frames.h qibao_frame_0_jpg
"""
import sys


def gen(src: str, out: str, name: str, append: bool):
    with open(src, "rb") as f:
        data = f.read()
    mode = "a" if append else "w"
    with open(out, mode) as f:
        if not append:
            f.write("// 由 tools/jpg2c.py 自动生成，勿手工编辑\n")
            f.write("#pragma once\n\n")
            f.write("#include <stddef.h>\n\n")
        f.write(f"static const unsigned char {name}[] = {{\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("    " + "".join(f"0x{b:02X}," for b in chunk) + "\n")
        f.write("};\n\n")
    print(f"OK: {src} -> {out} [{name}] {len(data)} bytes")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    gen(sys.argv[1], sys.argv[2], sys.argv[3], False)


if __name__ == "__main__":
    main()
