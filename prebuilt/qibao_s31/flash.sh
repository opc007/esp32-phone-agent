#!/bin/bash
# 固化版本只烧 app（0x20000）。
# 不写 bootloader、分区表、ota_data、assets，不擦 NVS，TF 卡上的人脸和模型保持不动。
set -euo pipefail
PORT="${1:-}"
BAUD="${2:-460800}"
DIR="$(cd "$(dirname "$0")" && pwd)"
ESPTOOL="${ESPTOOL:-$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/esptool}"

if [[ -z "$PORT" ]]; then
  echo "用法: $0 /dev/cu.usbserial-XXXX" >&2
  echo "先确认这个口的 CP2102N 序列号是 cc310f7eb6bcf011a6c7177148e9de0f。不要把 usbmodem 传进来。" >&2
  exit 1
fi
if [[ "$PORT" == *usbmodem* ]]; then
  echo "拒绝烧录：usbmodem 不是这块七宝。" >&2
  exit 1
fi

echo "==> 只烧 app 到 $PORT 的 0x20000（NVS、启动链、资源分区都不写）"
"$ESPTOOL" --chip esp32s31 --port "$PORT" --baud "$BAUD" \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x20000 "$DIR/xiaozhi.bin"
echo "==> 完成。核对大小应为 7080416 字节。"
