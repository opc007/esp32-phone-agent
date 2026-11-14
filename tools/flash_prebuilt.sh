#!/bin/bash
# 把 prebuilt/ 里的预编译固件烧录到 ESP32-S3。
# 只写 bootloader/分区表/otadata/app/assets，不碰 NVS（保留 WiFi 配网与账号绑定）。
#
# 用法：./flash_prebuilt.sh [串口] [波特率]
#   macOS 串口一般是 /dev/cu.usbserial-XXXX，Linux 是 /dev/ttyUSB0
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-/dev/cu.usbserial-2110}"
BAUD="${2:-460800}"

# 优先用系统 esptool，没有就现场 pip 装
ESPTOOL="$(command -v esptool || true)"
if [ -z "$ESPTOOL" ]; then
  echo "未找到 esptool，正在安装……"
  python3 -m pip install --user esptool >/dev/null
  ESPTOOL="$(python3 -c 'import esptool, sys; print(sys.executable)' | head -1)"
  ESPTOOL="$ESPTOOL -m esptool"
fi

echo "==> 烧录到 $PORT @ $BAUD（NVS 保留，配网不受影响）"
$ESPTOOL --chip esp32s3 --port "$PORT" --baud "$BAUD" \
  --before default_reset --after hard_reset \
  write_flash -z \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0      "$HERE/../prebuilt/bootloader.bin" \
  0x8000   "$HERE/../prebuilt/partition-table.bin" \
  0xd000   "$HERE/../prebuilt/ota_data_initial.bin" \
  0x20000  "$HERE/../prebuilt/xiaozhi.bin" \
  0x800000 "$HERE/../prebuilt/generated_assets.bin"
echo "==> 烧录完成。打开串口监视器（115200）可以看到「手机接入引导页: http://<ESP32 IP>/」"
