"""ESP32-S3 手机代理 —— 上电入口（MicroPython）。

流程：连手机热点 → 找到 Portal → 调 DeepSeek 决策 → 操作手机 → 打印结果。
任务结束后：按 BOOT 键可再跑一次（继续按 BOOT 运行中可急停）。
"""
import time

import network

import config
import agent
import portal as portal_mod
from llm import DeepSeek

try:
    from machine import Pin
    _BTN = Pin(0, Pin.IN, Pin.PULL_UP)
except Exception:
    _BTN = None


def connect_wifi(timeout=20, attempts=3):
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    for attempt in range(attempts):
        if wlan.isconnected():
            break
        print("[wifi] 连接 %s (第 %d 次)..." % (config.WIFI_SSID, attempt + 1))
        try:
            wlan.connect(config.WIFI_SSID, config.WIFI_PASSWORD)
        except Exception as exc:
            print("[wifi] connect() 失败:", exc)
        started = time.time()
        while not wlan.isconnected():
            if time.time() - started > timeout:
                break
            time.sleep(0.5)
        if wlan.isconnected():
            break
        try:
            wlan.active(False)
            time.sleep(1)
            wlan.active(True)
        except Exception:
            pass
    if not wlan.isconnected():
        raise OSError("wifi timeout")
    ip, mask, gw, dns = wlan.ifconfig()
    print("[wifi] ip=%s gw=%s" % (ip, gw))
    return wlan, gw


def wait_boot_press():
    """跑完等待 BOOT 键；没有按键（PC 环境）直接返回 False。"""
    if _BTN is None:
        return False
    print("[idle] 按 BOOT 键再跑一次任务（或断电重启）")
    while _BTN.value() == 1:
        time.sleep(0.1)
    while _BTN.value() == 0:
        time.sleep(0.05)
    print("[idle] 检测到 BOOT 按下，重新执行")
    return True


def main():
    while True:
        try:
            wlan, gateway = connect_wifi()
        except Exception as exc:
            print("[fatal] WiFi 连接失败:", exc)
            print("[hint] 确认手机热点已开启，SSID/密码与 config.py 一致")
            if not wait_boot_press():
                return
            continue

        p = portal_mod.Portal(gateway, config.PORTAL_PORT, config.PORTAL_TOKEN)
        ready = False
        for _attempt in range(6):
            try:
                print("[portal] version:", p.version())
                ready = True
                break
            except Exception as exc:
                print("[portal] 不可达 (%s)，重试" % exc)
                time.sleep(1)
        if not ready:
            print("[fatal] 连不上 Portal：确认手机热点、Portal 无障碍是否开启")
            if not wait_boot_press():
                return
            continue

        llm = DeepSeek(config.DEEPSEEK_API_KEY, config.DEEPSEEK_MODEL)
        for attempt in range(2):
            print("[agent] 任务:", config.TASK, "(第 %d 次)" % (attempt + 1))
            try:
                result = agent.run(p, llm, config.TASK, config.MAX_STEPS)
            except Exception as exc:
                result = "运行异常: %s" % exc
            print("[agent] 结果:", result)
            if result and not result.startswith(("达到最大步数", "失败", "运行异常")):
                break
            if attempt == 0:
                print("[agent] 未成功，回到桌面后重试一次")
                try:
                    p.key(portal_mod.KEY_HOME)
                except Exception:
                    pass
                time.sleep(2)

        if not wait_boot_press():
            return


main()
