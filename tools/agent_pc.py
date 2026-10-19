"""PC 端原型：用与固件相同的 agent/portal/llm 逻辑操作真机。

用途：在 ESP32 烧录前验证 prompt / 动作循环是否可靠。
运行前需要：
  1. 手机连 USB，Portal 无障碍已开启
  2. adb forward tcp:18080 tcp:8080
运行：
  python tools/agent_pc.py "任务文本"
"""
import os
import ssl
import sys

FIRMWARE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware")
sys.path.insert(0, os.path.abspath(FIRMWARE))

# PC 的 Python 3.12 移除了 ssl.wrap_socket，给固件代码补一个等价实现
if not hasattr(ssl, "wrap_socket"):
    def _wrap_socket(sock, server_hostname=None, **kwargs):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx.wrap_socket(sock, server_hostname=server_hostname)
    ssl.wrap_socket = _wrap_socket

import agent  # noqa: E402
import config  # noqa: E402
import portal  # noqa: E402
from llm import DeepSeek  # noqa: E402


def main():
    task = " ".join(sys.argv[1:]) or "打开设置，找到 Android 版本并告诉我（不要改任何设置）"
    p = portal.Portal("127.0.0.1", 18080, config.PORTAL_TOKEN)
    print("[pc] portal version:", p.version())
    model = os.environ.get("AGENT_MODEL") or config.DEEPSEEK_MODEL
    steps = int(os.environ.get("AGENT_STEPS") or 12)
    print("[pc] model:", model, "max_steps:", steps)
    llm = DeepSeek(config.DEEPSEEK_API_KEY, model)
    print("[pc] task:", task)
    result = agent.run(p, llm, task, max_steps=steps)
    print("[pc] result:", result)


if __name__ == "__main__":
    main()
