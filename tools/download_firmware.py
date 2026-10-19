"""Download the MicroPython firmware images for ESP32-S3 into firmware/.

Usage: python tools/download_firmware.py

The images are published by the MicroPython project (MIT) at micropython.org.
"""
import os
import urllib.request

BASE = "https://micropython.org/resources/firmware/"
FILES = [
    "ESP32_GENERIC_S3-20260824-v1.29.0.bin",            # no PSRAM
    "ESP32_GENERIC_S3-SPIRAM_OCT-20260824-v1.29.0.bin", # 8 MB octal PSRAM
]

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(HERE), "firmware")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for name in FILES:
        path = os.path.join(OUT_DIR, name)
        if os.path.exists(path) and os.path.getsize(path) > 1_000_000:
            print("already present:", name)
            continue
        url = BASE + name
        print("downloading", url)
        req = urllib.request.Request(url, headers={"User-Agent": "esp32-phone-agent"})
        with urllib.request.urlopen(req, timeout=180) as resp:
            data = resp.read()
        with open(path, "wb") as fh:
            fh.write(data)
        print("saved", name, len(data), "bytes")


if __name__ == "__main__":
    main()
