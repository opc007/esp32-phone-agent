#!/bin/bash
# 把本项目对 xiaozhi-esp32 的改动应用到一个上游源码克隆里。
#
# 用法：./apply.sh /path/to/xiaozhi-esp32
#   上游仓库：https://github.com/78/xiaozhi-esp32
#   已验证基线：commit a73e9a7e（v2.5.x，board = bread-compact-wifi-s3cam）
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
UP="${1:-$HOME/esp/xiaozhi-esp32}"

if [ ! -f "$UP/main/CMakeLists.txt" ]; then
  echo "错误：$UP 不是 xiaozhi-esp32 源码目录"
  exit 1
fi

BOARD_DIR="$UP/main/boards/bread-compact-wifi-s3cam"
echo "==> 1/4 拷贝板级源码到 $BOARD_DIR"
cp "$HERE/board/"*.cc "$HERE/board/"*.h "$HERE/board/"*.c "$HERE/board/config.json" "$BOARD_DIR/"

echo "==> 2/4 应用上游集成补丁（application.cc / protocol.cc / CMakeLists.txt / idf_component.yml）"
git -C "$UP" apply --check "$HERE/patches/0001-integrate-phone-agent.patch" \
  && git -C "$UP" apply "$HERE/patches/0001-integrate-phone-agent.patch" \
  || echo "   （补丁可能已应用过，跳过）"

echo "==> 3/4 写入可复现的 sdkconfig（关闭蓝牙省 SRAM，给语音让内存）"
cp "$HERE/sdkconfig.board" "$UP/sdkconfig"

echo "==> 4/4 完成。接下来编译："
cat <<'EOF'
  cd <上游目录>
  # 安装 ESP-IDF v6.1 环境后：
  . $HOME/esp/esp-idf/export.sh
  idf.py -B build -DIDF_TARGET=esp32s3 \
        -DBOARD_NAME=bread-compact-wifi-s3cam build
  idf.py -B build -p <串口> flash
EOF
