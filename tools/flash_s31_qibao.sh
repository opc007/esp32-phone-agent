#!/bin/bash
# 烧录七宝妹妹固件到 ESP32-S31-Korvo-1（build_s31 构建产物）
# 只写 bootloader/分区表/otadata/app/assets，绝不碰 NVS（保留配网与人脸特征）
set -e

PORT="${1:-/dev/cu.usbserial-2110}"
BAUD="${2:-460800}"
APP_DIR="$HOME/esp/xiaozhi-esp32/build_s31"
ESPTOOL="$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/esptool"

echo "==> 烧录七宝固件到 $PORT @ $BAUD"
echo "    (NVS 分区不写入，配网/人脸特征保留)"
"$ESPTOOL" --chip esp32s31 --port "$PORT" --baud "$BAUD" \
  --before default_reset --after hard_reset \
  write_flash -z \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x2000   "$APP_DIR/bootloader/bootloader.bin" \
  0x8000   "$APP_DIR/partition_table/partition-table.bin" \
  0xd000   "$APP_DIR/ota_data_initial.bin" \
  0x20000  "$APP_DIR/xiaozhi.bin" \
  0x800000 "$APP_DIR/generated_assets.bin"
echo "==> 烧录完成"
