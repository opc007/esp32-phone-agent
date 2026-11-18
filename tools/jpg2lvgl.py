#!/usr/bin/env python3
"""
把 JPG 转成 LVGL 9 可直接显示的 RGB565 C 数组头文件。
依赖：macOS 自带 sips（转 BMP）+ Python 标准库，无需 PIL。

用法：
  python3 tools/jpg2lvgl.py <输入.jpg> <输出.h> <数组名> [目标边长]
例：
  python3 tools/jpg2lvgl.py assets/qibao_face480.jpg \
    ~/esp/xiaozhi-esp32/main/boards/espressif/esp32-s31-korvo-1/qibao_avatar.h \
    qibao_avatar 360
"""
import struct
import subprocess
import sys
import tempfile
import os


def jpg_to_bmp(src: str, size: int, bmp_path: str):
    # sips 缩放（保持正方形裁切中心）+ 转 24bit BMP
    subprocess.run(["sips", "-z", str(size), str(size), src,
                    "--out", bmp_path], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["sips", "-s", "format", "bmp", bmp_path,
                    "--out", bmp_path], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_bmp_rgb565(bmp_path: str):
    with open(bmp_path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height_raw = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    height = abs(height_raw)
    if bpp != 24:
        raise ValueError(f"need 24bpp BMP, got {bpp}")
    row_bytes = ((width * 3 + 3) // 4) * 4  # BMP 行按 4 字节对齐
    bottom_up = height_raw > 0

    pixels = []  # 按 LVGL 需要的自上而下顺序
    for row in range(height):
        src_row = (height - 1 - row) if bottom_up else row
        line = []
        base = offset + src_row * row_bytes
        for col in range(width):
            b, g, r = data[base + col * 3: base + col * 3 + 3]
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            line.append(rgb565)
        pixels.extend(line)
    return width, height, pixels


def gen_header(name: str, width: int, height: int, pixels, out_path: str):
    with open(out_path, "w") as f:
        f.write("// 由 tools/jpg2lvgl.py 自动生成，勿手工编辑\n")
        f.write("#pragma once\n\n")
        f.write("#include <lvgl.h>\n\n")
        f.write(f"static const uint16_t {name}_map[] = {{\n")
        for i in range(0, len(pixels), 12):
            chunk = pixels[i:i + 12]
            f.write("    " + "".join(f"0x{v:04X}, " for v in chunk) + "\n")
        f.write("};\n\n")
        f.write(f"static const lv_image_dsc_t {name} = {{\n")
        f.write("    .header = {\n")
        f.write(f"        .magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write(f"        .cf = LV_COLOR_FORMAT_RGB565,\n")
        f.write(f"        .w = {width},\n")
        f.write(f"        .h = {height},\n")
        f.write(f"        .stride = {width * 2},\n")
        f.write("    },\n")
        f.write(f"    .data_size = sizeof({name}_map),\n")
        f.write(f"    .data = (const uint8_t*){name}_map,\n")
        f.write("};\n")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
    size = int(sys.argv[4]) if len(sys.argv) > 4 else 360
    with tempfile.TemporaryDirectory() as td:
        bmp = os.path.join(td, "img.bmp")
        jpg_to_bmp(src, size, bmp)
        w, h, px = read_bmp_rgb565(bmp)
    gen_header(name, w, h, px, out)
    print(f"OK: {src} -> {out} ({w}x{h}, {len(px)*2} bytes)")


if __name__ == "__main__":
    main()
