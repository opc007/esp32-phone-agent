#!/bin/bash
# 编译七宝妹妹固件（ESP32-S31-Korvo-1）
# 基于 build_direct.sh，改成 S31 target + 独立 build 目录 + 独立 sdkconfig
set -o pipefail
export TMPDIR=$HOME/.tmp_idf
mkdir -p "$TMPDIR"
export IDF_PATH=$HOME/esp/esp-idf-v6.1
export IDF_TOOLS_PATH=$HOME/.espressif
export ESP_IDF_VERSION=6.1.0
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env/idf6.1_py3.13_env"

VENV_PY="$IDF_TOOLS_PATH/python_env/idf6.1_py3.13_env/bin/python"
VENV_BIN="$IDF_TOOLS_PATH/python_env/idf6.1_py3.13_env/bin"
PYBIN="/Users/mac007/.workbuddy/binaries/python/versions/3.13.12/bin"
[ -x "$PYBIN/cmake" ] || PYBIN="$(dirname "$(command -v python3)")"

TOOLCHAIN_BINS=$(ls -d "$IDF_TOOLS_PATH"/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin 2>/dev/null | head -1)
RISCV_BINS=$(ls -d "$IDF_TOOLS_PATH"/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin 2>/dev/null | head -1)
export PATH="$VENV_BIN:$PYBIN:$TOOLCHAIN_BINS:$RISCV_BINS:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

cd "$HOME/esp/xiaozhi-esp32" || exit 1

BUILD_DIR="$HOME/esp/xiaozhi-esp32/build_s31"
SDKCONFIG="$BUILD_DIR/sdkconfig"

echo "[build_s31] venv python: $VENV_PY"
"$VENV_PY" -c "import rich_click; print('[build_s31] rich_click ok')"

exec "$VENV_PY" "$IDF_PATH/tools/idf.py" \
  -B "$BUILD_DIR" \
  -DIDF_TARGET=esp32s31 \
  -DSDKCONFIG="$SDKCONFIG" \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s31" \
  -DBOARD_NAME=esp32-s31-korvo-1 \
  reconfigure build
