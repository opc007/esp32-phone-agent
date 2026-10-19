# ESP32-S3 Phone Agent - user configuration
# Copy this file to config.py and fill in your own values:
#   cp firmware/config.example.py firmware/config.py   (or copy on Windows)
# config.py is git-ignored; never commit real secrets.

# --- Phone hotspot (the ESP32 joins it as a client) -----------------------
# On the phone: enable Personal Hotspot, use a 2.4 GHz band (ESP32-S3 has no 5 GHz),
# set SSID/password to match these values.
WIFI_SSID = "CHANGE_ME"
WIFI_PASSWORD = "CHANGE_ME_8_CHARS"

# --- Mobilerun Portal (Android app already installed on the phone) --------
PORTAL_PORT = 8080
# Bearer token of the Portal app. It is stable for one installation and only
# changes when the Portal app is reinstalled. Read it once over adb:
#   adb shell content query --uri content://com.mobilerun.portal/auth_token
PORTAL_TOKEN = "CHANGE_ME_36_CHARS"

# --- DeepSeek API ---------------------------------------------------------
DEEPSEEK_API_KEY = "sk-CHANGE_ME"
# deepseek-v4-pro (better planning, slower) or deepseek-flash (faster, weaker)
DEEPSEEK_MODEL = "deepseek-v4-pro"

# --- Task ----------------------------------------------------------------
# The board runs this task once after boot.
TASK = "打开设置，找到 Android 版本并告诉我（不要改任何设置）"
MAX_STEPS = 18

# Serial debug output
DEBUG = True
