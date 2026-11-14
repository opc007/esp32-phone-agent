#include "phone_hid.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_mac.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tusb.h"
#include "tinyusb.h"

extern const tinyusb_config_t phone_hid_tusb_cfg;

#define TAG "PhoneHid"

namespace {

SemaphoreHandle_t s_mutex = nullptr;

// HID 键盘 modifier
constexpr uint8_t MOD_LSHIFT = 0x02;
constexpr uint8_t MOD_LALT   = 0x04;
constexpr uint8_t MOD_LGUI   = 0x08;

// 普通字符 -> HID usage（美式键盘布局）
uint8_t AsciiToUsage(char c, uint8_t& modifier) {
    modifier = 0;
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') { modifier = MOD_LSHIFT; return 0x04 + (c - 'A'); }
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    if (c == '0') return 0x27;
    switch (c) {
        case ' ':  return 0x2C;
        case '\n': return 0x28;  // Enter
        case '\r': return 0;     // 忽略
        case '\t': return 0x2B;
        case '\b': return 0x2A;  // Backspace
        case 0x1B: return 0x29;  // Esc
        case '-':  return 0x2D;
        case '=':  return 0x2E;
        case '[':  return 0x2F;
        case ']':  return 0x30;
        case '\\': return 0x31;
        case ';':  return 0x33;
        case '\'': return 0x34;
        case '`':  return 0x35;
        case ',':  return 0x36;
        case '.':  return 0x37;
        case '/':  return 0x38;
        case '!':  modifier = MOD_LSHIFT; return 0x1E;
        case '@':  modifier = MOD_LSHIFT; return 0x1F;
        case '#':  modifier = MOD_LSHIFT; return 0x20;
        case '$':  modifier = MOD_LSHIFT; return 0x21;
        case '%':  modifier = MOD_LSHIFT; return 0x22;
        case '^':  modifier = MOD_LSHIFT; return 0x23;
        case '&':  modifier = MOD_LSHIFT; return 0x24;
        case '*':  modifier = MOD_LSHIFT; return 0x25;
        case '(':  modifier = MOD_LSHIFT; return 0x26;
        case ')':  modifier = MOD_LSHIFT; return 0x27;
        case '_':  modifier = MOD_LSHIFT; return 0x2D;
        case '+':  modifier = MOD_LSHIFT; return 0x2E;
        case '{':  modifier = MOD_LSHIFT; return 0x2F;
        case '}':  modifier = MOD_LSHIFT; return 0x30;
        case '|':  modifier = MOD_LSHIFT; return 0x31;
        case ':':  modifier = MOD_LSHIFT; return 0x33;
        case '"':  modifier = MOD_LSHIFT; return 0x34;
        case '~':  modifier = MOD_LSHIFT; return 0x35;
        case '<':  modifier = MOD_LSHIFT; return 0x36;
        case '>':  modifier = MOD_LSHIFT; return 0x37;
        case '?':  modifier = MOD_LSHIFT; return 0x38;
        default:   return 0;
    }
}

int Clip(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int NormToRaw(int v) {  // 0..1000 -> 0..4095
    return Clip(v, 0, 1000) * 4095 / 1000;
}

}  // namespace

PhoneHid& PhoneHid::GetInstance() {
    static PhoneHid instance;
    return instance;
}

void PhoneHid::Init() {
    if (inited_) return;
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    // 用 efuse MAC 的后 4 字节生成序列号，避免多台设备冲突
    static char serial[13];
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // 覆盖描述符里的序列号
    phone_hid_tusb_cfg.string_descriptor[3] = serial;

    ESP_ERROR_CHECK(tinyusb_driver_install(&phone_hid_tusb_cfg));
    inited_ = true;
    ESP_LOGI(TAG, "USB HID 初始化完成 (键盘+鼠标+触摸屏+多媒体)");
}

bool PhoneHid::IsMounted() {
    return inited_ && tud_ready() && tud_hid_ready();
}

bool PhoneHid::SendReport(uint8_t report_id, const void* data, uint16_t len) {
    if (!inited_) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    bool ok = false;
    for (int i = 0; i < 100; i++) {   // 最多等 ~500ms 让端点空闲
        if (tud_hid_n_report(0, report_id, data, len)) { ok = true; break; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    xSemaphoreGive(s_mutex);
    if (!ok) ESP_LOGW(TAG, "HID report %u 发送失败", report_id);
    return ok;
}

bool PhoneHid::SendKeyboard(uint8_t modifier, const uint8_t key[6]) {
    uint8_t report[8] = {modifier, 0, key[0], key[1], key[2], key[3], key[4], key[5]};
    return SendReport(1, report, sizeof(report));
}

void PhoneHid::DelayMs(int ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

bool PhoneHid::TypeText(const std::string& text, int per_char_delay_ms) {
    uint8_t zeros[6] = {0};
    for (size_t i = 0; i < text.size() && i < 256; i++) {
        char c = text[i];
        // UTF-8 多字节（中文等）：跳过并提示
        if ((unsigned char)c >= 0x80) {
            ESP_LOGW(TAG, "TypeText 收到非 ASCII 字符，已跳过");
            while (i + 1 < text.size() && ((unsigned char)text[i + 1] & 0xC0) == 0x80) i++;
            continue;
        }
        uint8_t modifier = 0;
        uint8_t usage = AsciiToUsage(c, modifier);
        if (usage == 0) continue;
        SendKeyboard(0, zeros);  // 释放旧键
        if (!SendKeyboard(modifier, (uint8_t[6]){usage, 0, 0, 0, 0, 0})) return false;
        DelayMs(per_char_delay_ms);
        SendKeyboard(0, zeros);
        DelayMs(per_char_delay_ms);
    }
    return true;
}

bool PhoneHid::PressKey(uint8_t usage, int hold_ms) {
    uint8_t zeros[6] = {0};
    SendKeyboard(0, zeros);
    if (!SendKeyboard(0, (uint8_t[6]){usage, 0, 0, 0, 0, 0})) return false;
    DelayMs(hold_ms);
    return SendKeyboard(0, zeros);
}

bool PhoneHid::PressCombo(uint8_t modifier, uint8_t usage, int hold_ms) {
    uint8_t zeros[6] = {0};
    SendKeyboard(0, zeros);
    if (!SendKeyboard(modifier, (uint8_t[6]){usage, 0, 0, 0, 0, 0})) return false;
    DelayMs(hold_ms);
    return SendKeyboard(0, zeros);
}

bool PhoneHid::MouseMove(int dx, int dy) {
    int steps = (std::max(std::abs(dx), std::abs(dy)) + 99) / 100;
    if (steps < 1) steps = 1;
    for (int i = 0; i < steps; i++) {
        int sx = Clip((long)dx * (i + 1) / steps - (long)dx * i / steps, -127, 127);
        int sy = Clip((long)dy * (i + 1) / steps - (long)dy * i / steps, -127, 127);
        uint8_t report[5] = {0, (uint8_t)sx, (uint8_t)sy, 0};
        if (!SendReport(2, report, 4)) return false;
        DelayMs(4);
    }
    return true;
}

bool PhoneHid::MouseClick(uint8_t button) {
    uint8_t down[4] = {button, 0, 0, 0};
    uint8_t up[4] = {0, 0, 0, 0};
    if (!SendReport(2, down, 4)) return false;
    DelayMs(60);
    return SendReport(2, up, 4);
}

bool PhoneHid::MouseScroll(int8_t wheel) {
    uint8_t report[4] = {0, 0, 0, (uint8_t)wheel};
    bool ok = SendReport(2, report, 4);
    DelayMs(20);
    uint8_t up[4] = {0, 0, 0, 0};
    SendReport(2, up, 4);
    return ok;
}

bool PhoneHid::TouchTap(int x, int y, int hold_ms) {
    int rx = NormToRaw(x), ry = NormToRaw(y);
    // 先悬停到目标位置（无触摸）
    uint8_t hover[6] = {0, (uint8_t)(rx & 0xFF), (uint8_t)(rx >> 8),
                            (uint8_t)(ry & 0xFF), (uint8_t)(ry >> 8), 0};
    SendReport(3, hover, 6);
    DelayMs(20);
    // 按下
    uint8_t down[6] = {0, hover[1], hover[2], hover[3], hover[4], 0x01};
    if (!SendReport(3, down, 6)) return false;
    DelayMs(hold_ms);
    // 抬起
    return SendReport(3, hover, 6);
}

bool PhoneHid::TouchSwipe(int x1, int y1, int x2, int y2, int duration_ms) {
    int steps = Clip(duration_ms / 12, 6, 60);
    int rx1 = NormToRaw(x1), ry1 = NormToRaw(y1);
    int rx2 = NormToRaw(x2), ry2 = NormToRaw(y2);

    uint8_t hover[6] = {0, (uint8_t)(rx1 & 0xFF), (uint8_t)(rx1 >> 8),
                            (uint8_t)(ry1 & 0xFF), (uint8_t)(ry1 >> 8), 0};
    SendReport(3, hover, 6);
    DelayMs(20);

    // 按下起点
    uint8_t down[6] = {0, hover[1], hover[2], hover[3], hover[4], 0x01};
    if (!SendReport(3, down, 6)) return false;
    DelayMs(40);

    for (int i = 1; i <= steps; i++) {
        int rx = rx1 + (long)(rx2 - rx1) * i / steps;
        int ry = ry1 + (long)(ry2 - ry1) * i / steps;
        uint8_t move[6] = {0, (uint8_t)(rx & 0xFF), (uint8_t)(rx >> 8),
                               (uint8_t)(ry & 0xFF), (uint8_t)(ry >> 8), 0x01};
        if (!SendReport(3, move, 6)) return false;
        DelayMs(duration_ms / steps);
    }
    // 抬起
    uint8_t up[6] = {0, (uint8_t)(rx2 & 0xFF), (uint8_t)(rx2 >> 8),
                         (uint8_t)(ry2 & 0xFF), (uint8_t)(ry2 >> 8), 0};
    return SendReport(3, up, 6);
}

bool PhoneHid::ConsumerKey(uint16_t usage, int hold_ms) {
    uint8_t down[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
    if (!SendReport(4, down, 2)) return false;
    DelayMs(hold_ms);
    uint8_t up[2] = {0, 0};
    return SendReport(4, up, 2);
}
