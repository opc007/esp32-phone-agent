#!/bin/bash
# 烧录七宝妹妹固件到 ESP32-S31-Korvo-1（不碰 NVS，保留配网/人脸特征）
set -e
PORT="${1:-/dev/cu.usbserial-2110}"
BAUD="${2:-460800}"
DIR="$(cd "$(dirname "$0")" && pwd)"
ESPTOOL="${ESPTOOL:-$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/esptool}"
echo "==> 烧录七宝固件到 $PORT（NVS 不写入，配网/人脸特征保留）"
"$ESPTOOL" --chip esp32s31 --port "$PORT" --baud "$BAUD" \
  --before default_reset --after hard_reset \
  write_flash -z --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x2000   "$DIR/bootloader.bin" \
  0x8000   "$DIR/partition-table.bin" \
  0xd000   "$DIR/ota_data_initial.bin" \
  0x20000  "$DIR/xiaozhi.bin" \
  0x800000 "$DIR/generated_assets.bin"
echo "==> 完成"
