#!/bin/bash
# 在电脑（Mac/Linux）上启动 ESP32 的控制入口（HTTP Portal）。
# 说明：ESP32 直连手机方案【不需要】运行本脚本；只有想用「语音控制电脑」
# （打开电脑 App / 发消息 / 点歌 / 控制浏览器）时才需要它。
#
#   ./start_computer_portal.sh            # 默认 0.0.0.0:8080，令牌 test
#   ./start_computer_portal.sh 8080 mytoken
#
# 启动后，在 ESP32 串口执行：macset <本机IP>:8080 <令牌> 即可切换目标。
set -e
TOOLS="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-8080}"
TOKEN="${2:-test}"
PY="$(command -v python3)"

IP="$( (ipconfig getifaddr en0 2>/dev/null || hostname -I 2>/dev/null) | awk '{print $1}')"
echo "本机局域网 IP: ${IP:-未知}"
echo "Portal 监听 0.0.0.0:$PORT，令牌=$TOKEN"
echo "ESP32 串口切换目标：macset ${IP:-<本机IP>}:$PORT $TOKEN"
echo "按 Ctrl+C 停止服务"
echo
exec "$PY" -u "$TOOLS/computer_portal.py" "$PORT" "$TOKEN"
