#include "phone_ble_hid.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_bt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_hidd.h"
#include "esp_hid_gap.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#endif

#define TAG "PhoneBleHid"

namespace {

// 报告映射：键盘(1) / 鼠标(2) / 多媒体(3) / 触摸屏(4)
const uint8_t kReportMap[] = {
    // ---- 键盘 ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array)
    0xC0,              // End Collection

    // ---- 鼠标 ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Buttons)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Const)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0xC0,              //   End Collection
    0xC0,              // End Collection

    // ---- 多媒体键 ----
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data,Array)
    0xC0,              // End Collection

    // ---- 触摸屏（绝对坐标 + 按下标志）----
    0x05, 0x0D,        // Usage Page (Digitizer)
    0x09, 0x04,        // Usage (Touch Screen)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x04,        //   Report ID (4)
    0x09, 0x22,        //   Usage (Finger)
    0xA1, 0x02,        //   Collection (Logical)
    0x05, 0x0D,        //     Usage Page (Digitizer)
    0x09, 0x42,        //     Usage (Tip Switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x75, 0x07,        //     Report Size (7)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x0F,  //     Logical Maximum (4095)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x0F,  //     Logical Maximum (4095)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

esp_hid_raw_report_map_t kReportMaps[] = {
    { .data = kReportMap, .len = sizeof(kReportMap) },
};

char s_serial[16] = "XIAOZHI000000";

esp_hid_device_config_t kDeviceConfig = {};

SemaphoreHandle_t s_mutex = nullptr;
volatile bool s_ready = false;
volatile bool s_connected = false;
esp_hidd_dev_t* s_dev = nullptr;

int Clip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int Norm(int v) { return Clip(v, 0, 1000) * 4095 / 1000; }

void DelayMs(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

}  // namespace

PhoneBleHid& PhoneBleHid::GetInstance() {
    static PhoneBleHid instance;
    return instance;
}

void PhoneBleHid::Ready(bool connected) {
    s_connected = connected;
    s_ready = true;
    ESP_LOGI(TAG, "蓝牙 HID %s", connected ? "已连接 ✓" : "已断开（重新广播）");
}

#if CONFIG_BT_NIMBLE_ENABLED
static void BleHostTask(void*) {
    ESP_LOGI(TAG, "NimBLE 主机任务启动");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

extern "C" {
void ble_store_config_init(void);
// esp_hid_gap.c 在连接事件里会调用它启动示例任务，这里留空即可
void ble_hid_task_start_up(void) { ESP_LOGI(TAG, "BLE HID 链路已建立"); }
}

// NimBLE 主机同步完成后才能真正发广播（esp_hid_gap.c 里没有注册 sync_cb，
// 必须在这里补上，否则设备永远不会出现在手机的蓝牙列表里）
static void OnBleSync(void) {
    ESP_LOGI(TAG, "NimBLE 主机同步完成 → 启动广播");
    // 探一下本机地址类型（诊断用），否则 ble_gap_adv_start 可能因地址无效返回 EINVAL
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    int arc = ble_hs_id_infer_auto(0, &own_addr_type);
    ESP_LOGI(TAG, "地址推断: rc=%d type=%u", arc, own_addr_type);
    esp_err_t err = esp_hid_ble_gap_adv_start();
    ESP_LOGI(TAG, "广播启动结果: %s", esp_err_to_name(err));
}

static void HidEventCb(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data) {
    auto* param = (esp_hidd_event_data_t*)event_data;
    switch (id) {
        case ESP_HIDD_START_EVENT:
            ESP_LOGI(TAG, "HID 协议栈已启动");
            break;
        case ESP_HIDD_CONNECT_EVENT:
            s_connected = true;
            ESP_LOGI(TAG, "手机已连接蓝牙 HID ✓ (HID 层 connect 事件已收到)");
            break;
        case ESP_HIDD_DISCONNECT_EVENT:
            s_connected = false;
            ESP_LOGW(TAG, "蓝牙断开(reason=%d)，重新开始广播", param->disconnect.reason);
            esp_hid_ble_gap_adv_start();
            break;
        case ESP_HIDD_PROTOCOL_MODE_EVENT:
            // 0=boot 协议(只认键盘/鼠标)，1=report 协议(4 个报告都可用)
            ESP_LOGI(TAG, "主机设置协议模式 = %d (%s)",
                     (int)param->protocol_mode.protocol_mode,
                     param->protocol_mode.protocol_mode == 0 ? "BOOT：绝对坐标触摸报告会被忽略"
                                                            : "REPORT：全部报告可用");
            break;
        case ESP_HIDD_CONTROL_EVENT:
            ESP_LOGI(TAG, "HID 控制点 = %d (0x03=暂停, 0x04=退出暂停)", (int)param->control.control);
            break;
        default:
            break;
    }
    (void)param;
}
#endif

void PhoneBleHid::StartTask() {
    if (inited_) return;
    inited_ = true;
    s_mutex = xSemaphoreCreateMutex();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memset(&kDeviceConfig, 0, sizeof(kDeviceConfig));
    kDeviceConfig.vendor_id = 0x16C0;
    kDeviceConfig.product_id = 0x05DF;
    kDeviceConfig.version = 0x0100;
    kDeviceConfig.device_name = "XiaoZhi Controller";
    kDeviceConfig.manufacturer_name = "Espressif";
    kDeviceConfig.serial_number = s_serial;
    kDeviceConfig.report_maps = kReportMaps;
    kDeviceConfig.report_maps_len = 1;

#if CONFIG_BT_NIMBLE_ENABLED
    xTaskCreate([](void*) {
        ESP_LOGI(TAG, "初始化 BLE HID …");
        esp_err_t err = esp_hid_gap_init(ESP_BT_MODE_BLE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hid_gap_init 失败: %s", esp_err_to_name(err));
            vTaskDelete(nullptr);
            return;
        }
        ESP_ERROR_CHECK(esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD,
                                                 kDeviceConfig.device_name));
        ESP_ERROR_CHECK(esp_hidd_dev_init(&kDeviceConfig, ESP_HID_TRANSPORT_BLE,
                                          HidEventCb, &s_dev));
        esp_hidd_dev_battery_set(s_dev, 100);

        // 手机端用 Just Works 配对：我们没有“输入/显示配对码”的交互界面，
        // 若保持 esp_hid_gap.c 默认的 DISP_ONLY，手机会等一个永远不出现的 6 位码。
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sync_cb = OnBleSync;

        ble_store_config_init();
        nimble_port_freertos_init(BleHostTask);
        s_ready = true;
        ESP_LOGI(TAG, "BLE HID 就绪，等待手机配对（设备名: XiaoZhi Controller）");
        vTaskDelete(nullptr);
    }, "ble_hid_init", 6144, nullptr, 5, nullptr);
#else
    ESP_LOGW(TAG, "未启用 NimBLE，蓝牙 HID 不可用");
#endif
}

void PhoneBleHid::Init() { StartTask(); }

bool PhoneBleHid::IsConnected() { return s_connected && s_dev != nullptr; }
bool PhoneBleHid::IsReady() { return s_ready; }

// 把每个 HID 报告的原始字节回显到 UART0。
// 用途：GPIO19/20 被 LCD 占用，板子上的 USB-串口芯片是唯一可用的有线通道。
// Mac/PC 侧运行 tools/serial_hid_bridge.py 就能把这些报告解析成真实键鼠事件，
// 即使蓝牙没有配对，也能对整条链路做端到端实测。
#ifndef PHONE_HID_SERIAL_ECHO
#define PHONE_HID_SERIAL_ECHO 1
#endif
#if PHONE_HID_SERIAL_ECHO
static void EchoReport(uint8_t id, const uint8_t* data, size_t len) {
    char hex[65] = {0};
    size_t n = len > 32 ? 32 : len;
    for (size_t i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02X", data[i]);
    ESP_LOGI(TAG, "[[HID:%u:%s]]", (unsigned)id, hex);
}
#endif

bool PhoneBleHid::Send(uint8_t report_id, const uint8_t* data, size_t len) {
    // 注意：不再依赖 s_connected 这个"事件回调置位"的标志。
    // 若 ESP_HIDD_CONNECT_EVENT 因时序丢失，标志会一直是 false，所有动作静默失败。
    // 底层 nimble_hidd_dev_input_set 自己会检查 dev->connected，判断更可靠。
    if (!s_dev) {
        ESP_LOGW(TAG, "[发送] report %u 失败：HID 设备尚未初始化", report_id);
        return false;
    }
    if (report_id == 0 || data == nullptr) return true;   // 用作探针
#if PHONE_HID_SERIAL_ECHO
    EchoReport(report_id, data, len);
#endif
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "[发送] report %u 失败：拿不到发送锁", report_id);
        return false;
    }
    esp_err_t err = esp_hidd_dev_input_set(s_dev, 0, report_id, (uint8_t*)data, len);
    if (s_mutex) xSemaphoreGive(s_mutex);
    if (err != ESP_OK) {
        if (serial_bridge_) {
            static uint32_t bridged = 0;
            if (++bridged <= 3 || bridged % 50 == 0) {
                ESP_LOGI(TAG, "[串口桥] report %u 已由 UART0 送达（蓝牙未连接，累计 %u）",
                         report_id, bridged);
            }
            return true;
        }
        ESP_LOGW(TAG, "[发送] report %u (%u bytes) 失败: %s；当前标志 conn=%d",
                 report_id, (unsigned)len, esp_err_to_name(err), (int)s_connected);
    } else {
        static uint32_t ok_count = 0;
        if (++ok_count <= 6 || ok_count % 50 == 0) {
            ESP_LOGI(TAG, "[发送] report %u (%u bytes) 成功，累计 %u 次",
                     report_id, (unsigned)len, ok_count);
        }
    }
    return err == ESP_OK;
}

bool PhoneBleHid::TypeText(const std::string& text, int per_char_delay_ms) {
    static const uint8_t kZeros[6] = {0};
    for (size_t i = 0; i < text.size() && i < 256; i++) {
        char c = text[i];
        if ((unsigned char)c >= 0x80) continue;   // 非 ASCII 跳过（中文走 Portal）
        uint8_t mod = 0, usage = 0;
        if (c >= 'a' && c <= 'z') usage = 0x04 + (c - 'a');
        else if (c >= 'A' && c <= 'Z') { mod = 0x02; usage = 0x04 + (c - 'A'); }
        else if (c >= '1' && c <= '9') usage = 0x1E + (c - '1');
        else if (c == '0') usage = 0x27;
        else if (c == ' ') usage = 0x2C;
        else if (c == '\n') usage = 0x28;
        else if (c == '-') usage = 0x2D;
        else if (c == '.') usage = 0x37;
        else if (c == ',') usage = 0x36;
        else if (c == '/') usage = 0x38;
        else if (c == '!') { mod = 0x02; usage = 0x1E; }
        else if (c == '?') { mod = 0x02; usage = 0x38; }
        else if (c == ':') { mod = 0x02; usage = 0x33; }
        else if (c == '@') { mod = 0x02; usage = 0x1F; }
        if (!usage) continue;
        uint8_t down[8] = {mod, 0, usage, 0, 0, 0, 0, 0};
        if (!Send(1, down, 8)) return false;
        DelayMs(per_char_delay_ms);
        uint8_t up[8] = {0};
        Send(1, up, 8);
        DelayMs(per_char_delay_ms);
    }
    return true;
}

bool PhoneBleHid::PressKey(uint8_t usage, int hold_ms) {
    uint8_t zeros[8] = {0};
    Send(1, zeros, 8);
    uint8_t down[8] = {0, 0, usage, 0, 0, 0, 0, 0};
    if (!Send(1, down, 8)) return false;
    DelayMs(hold_ms);
    return Send(1, zeros, 8);
}

bool PhoneBleHid::PressCombo(uint8_t modifier, uint8_t usage, int hold_ms) {
    uint8_t zeros[8] = {0};
    Send(1, zeros, 8);
    uint8_t down[8] = {modifier, 0, usage, 0, 0, 0, 0, 0};
    if (!Send(1, down, 8)) return false;
    DelayMs(hold_ms);
    return Send(1, zeros, 8);
}

bool PhoneBleHid::MouseMove(int dx, int dy) {
    int steps = (std::max(std::abs(dx), std::abs(dy)) + 99) / 100;
    if (steps < 1) steps = 1;
    for (int i = 0; i < steps; i++) {
        int sx = Clip((long)dx * (i + 1) / steps - (long)dx * i / steps, -127, 127);
        int sy = Clip((long)dy * (i + 1) / steps - (long)dy * i / steps, -127, 127);
        uint8_t rep[4] = {0, (uint8_t)sx, (uint8_t)sy, 0};
        if (!Send(2, rep, 4)) return false;
        DelayMs(5);
    }
    return true;
}

bool PhoneBleHid::MouseClick(uint8_t button) {
    uint8_t down[4] = {button, 0, 0, 0};
    uint8_t up[4] = {0};
    if (!Send(2, down, 4)) return false;
    DelayMs(60);
    return Send(2, up, 4);
}

bool PhoneBleHid::MouseScroll(int8_t wheel) {
    uint8_t rep[4] = {0, 0, 0, (uint8_t)wheel};
    bool ok = Send(2, rep, 4);
    DelayMs(20);
    uint8_t up[4] = {0};
    Send(2, up, 4);
    return ok;
}

bool PhoneBleHid::TouchTap(int x, int y, int hold_ms) {
    int rx = Norm(x), ry = Norm(y);
    uint8_t hover[5] = {0, (uint8_t)(rx & 0xFF), (uint8_t)(rx >> 8),
                        (uint8_t)(ry & 0xFF), (uint8_t)(ry >> 8)};
    Send(4, hover, 5);
    DelayMs(20);
    uint8_t down[5] = {0x01, hover[1], hover[2], hover[3], hover[4]};
    if (!Send(4, down, 5)) return false;
    DelayMs(hold_ms);
    return Send(4, hover, 5);
}

bool PhoneBleHid::TouchSwipe(int x1, int y1, int x2, int y2, int duration_ms) {
    int steps = Clip(duration_ms / 12, 6, 60);
    int rx1 = Norm(x1), ry1 = Norm(y1), rx2 = Norm(x2), ry2 = Norm(y2);
    uint8_t hover[5] = {0, (uint8_t)(rx1 & 0xFF), (uint8_t)(rx1 >> 8),
                        (uint8_t)(ry1 & 0xFF), (uint8_t)(ry1 >> 8)};
    Send(4, hover, 5);
    DelayMs(20);
    uint8_t down[5] = {0x01, hover[1], hover[2], hover[3], hover[4]};
    if (!Send(4, down, 5)) return false;
    DelayMs(40);
    for (int i = 1; i <= steps; i++) {
        int rx = rx1 + (long)(rx2 - rx1) * i / steps;
        int ry = ry1 + (long)(ry2 - ry1) * i / steps;
        uint8_t mv[5] = {0x01, (uint8_t)(rx & 0xFF), (uint8_t)(rx >> 8),
                         (uint8_t)(ry & 0xFF), (uint8_t)(ry >> 8)};
        if (!Send(4, mv, 5)) return false;
        DelayMs(duration_ms / steps);
    }
    uint8_t up[5] = {0, (uint8_t)(rx2 & 0xFF), (uint8_t)(rx2 >> 8),
                     (uint8_t)(ry2 & 0xFF), (uint8_t)(ry2 >> 8)};
    return Send(4, up, 5);
}

bool PhoneBleHid::ConsumerKey(uint16_t usage, int hold_ms) {
    uint8_t down[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
    if (!Send(3, down, 2)) return false;
    DelayMs(hold_ms);
    uint8_t up[2] = {0, 0};
    return Send(3, up, 2);
}
