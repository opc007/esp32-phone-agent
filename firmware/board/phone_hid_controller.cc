/*
 * PhoneHidController —— 把手机控制能力注册成小智的 MCP 工具，
 * 让大模型在语音对话中直接调用。
 */
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_err.h>

#include "application.h"
#include "device_state.h"
#include "mcp_server.h"
#include "phone_ble_hid.h"
#include "phone_hid.h"
#include "portal_client.h"
#include "portal_tools.h"

// HID 键盘 usage
#define HID_KEY_ESC        0x29
#define HID_KEY_TAB        0x2B
#define HID_KEY_SPACE      0x2C
#define HID_KEY_RETURN     0x28
#define HID_KEY_BACKSPACE  0x2A
#define HID_KEY_DELETE     0x4C
#define HID_KEY_HOME       0x4A
#define HID_KEY_END        0x4D
#define HID_KEY_PAGE_UP    0x4B
#define HID_KEY_PAGE_DOWN  0x4E
#define HID_KEY_RIGHT      0x4F
#define HID_KEY_LEFT       0x50
#define HID_KEY_DOWN       0x51
#define HID_KEY_UP         0x52

#define MOD_LCTRL 0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT 0x04
#define MOD_LGUI 0x08

// 多媒体 usage (Consumer page)
#define CONSUMER_VOLUME_UP    0x00E9
#define CONSUMER_VOLUME_DOWN  0x00EA
#define CONSUMER_MUTE         0x00E8
#define CONSUMER_PLAY_PAUSE   0x00CD

#define TAG "PhoneHidCtl"

// USB HID 与屏幕共用 GPIO19/20（ESP32-S3 的 USB PHY 固定引脚）。
// 0 = 屏幕模式（USB 不初始化，控制走蓝牙 HID）；1 = USB 模式（屏幕不可用）。
#ifndef PHONE_HID_USE_USB
#define PHONE_HID_USE_USB 0
#endif

// 统一输入通道：蓝牙优先（无需插线、不可被 App 检测），其次 USB
//
// 【关键修复】屏幕模式（PHONE_HID_USE_USB=0）下 USB HID 根本没有初始化。
// 旧代码用 IsConnected() 事件标志决定走哪条通道，一旦连接事件时序有偏差，
// 就会退回一个从未初始化的 USB 通道，所有动作静默失败、连日志都没有。
// 现在屏幕模式下永远走蓝牙，由 IDF 底层判断真实连接状态。
class HidAny {
public:
    void Init() {
#if PHONE_HID_USE_USB
        PhoneHid::GetInstance().Init();
#endif
        PhoneBleHid::GetInstance().Init();
    }
    static bool UseBle() {
#if PHONE_HID_USE_USB
        return PhoneBleHid::GetInstance().IsConnected() || !PhoneHid::GetInstance().IsMounted();
#else
        return true;   // 屏幕模式下蓝牙是唯一输入通道
#endif
    }
    static const char* Channel() { return UseBle() ? "BLE" : "USB"; }
    bool IsMounted() {
        if (PhoneBleHid::GetInstance().IsConnected()) return true;
#if PHONE_HID_USE_USB
        if (PhoneHid::GetInstance().IsMounted()) return true;
#endif
        return false;
    }
    bool TypeText(const std::string& t, int d = 12) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().TypeText(t, d)
                           : PhoneHid::GetInstance().TypeText(t, d);
        ESP_LOGI(TAG, "[动作] type \"%s\" 通道=%s → %s", t.c_str(), Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool PressKey(uint8_t usage, int hold_ms = 30) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().PressKey(usage, hold_ms)
                           : PhoneHid::GetInstance().PressKey(usage, hold_ms);
        ESP_LOGI(TAG, "[动作] key 0x%02X 通道=%s → %s", (unsigned)usage, Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool PressCombo(uint8_t mod, uint8_t usage, int hold_ms = 40) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().PressCombo(mod, usage, hold_ms)
                           : PhoneHid::GetInstance().PressCombo(mod, usage, hold_ms);
        ESP_LOGI(TAG, "[动作] combo mod=0x%02X key=0x%02X 通道=%s → %s",
                 (unsigned)mod, (unsigned)usage, Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool MouseMove(int dx, int dy) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().MouseMove(dx, dy)
                           : PhoneHid::GetInstance().MouseMove(dx, dy);
        ESP_LOGI(TAG, "[动作] mouse_move(%d,%d) 通道=%s → %s", dx, dy, Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool MouseClick(uint8_t btn = 0x01) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().MouseClick(btn)
                           : PhoneHid::GetInstance().MouseClick(btn);
        ESP_LOGI(TAG, "[动作] mouse_click 通道=%s → %s", Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool MouseScroll(int8_t w) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().MouseScroll(w)
                           : PhoneHid::GetInstance().MouseScroll(w);
        ESP_LOGI(TAG, "[动作] scroll(%d) 通道=%s → %s", w, Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool TouchTap(int x, int y, int hold_ms = 60) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().TouchTap(x, y, hold_ms)
                           : PhoneHid::GetInstance().TouchTap(x, y, hold_ms);
        ESP_LOGI(TAG, "[动作] tap(%d,%d) 通道=%s → %s", x, y, Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool TouchSwipe(int x1, int y1, int x2, int y2, int dur = 400) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().TouchSwipe(x1, y1, x2, y2, dur)
                           : PhoneHid::GetInstance().TouchSwipe(x1, y1, x2, y2, dur);
        ESP_LOGI(TAG, "[动作] swipe(%d,%d→%d,%d) 通道=%s → %s",
                 x1, y1, x2, y2, Channel(), ok ? "OK" : "失败");
        return ok;
    }
    bool ConsumerKey(uint16_t usage, int hold_ms = 60) {
        bool ok = UseBle() ? PhoneBleHid::GetInstance().ConsumerKey(usage, hold_ms)
                           : PhoneHid::GetInstance().ConsumerKey(usage, hold_ms);
        ESP_LOGI(TAG, "[动作] consumer 0x%03X 通道=%s → %s", (unsigned)usage, Channel(), ok ? "OK" : "失败");
        return ok;
    }
};

inline HidAny& Hid() {
    static HidAny instance;
    return instance;
}

class PhoneHidController {
public:
    PhoneHidController();

private:
    void StartMonitorTask();
    static std::string Ok(const std::string& msg) {
        return "{\"success\": true, \"message\": \"" + msg + "\"}";
    }

    void RegisterTools() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool(
            "self.phone.tap",
            "在手机/电脑屏幕上点击指定位置。x、y 是屏幕相对坐标（0-1000）：左上角是(0,0)，右上角是(1000,0)，左下角(0,1000)，屏幕正中是(500,500)。\n"
            "已配置 Portal 时自动走安卓无障碍点击（最可靠）；未配置时走蓝牙 HID 绝对坐标触摸（部分安卓机型不支持）。\n"
            "用法示例：点击屏幕中间偏下的按钮 → x=500, y=750。屏幕底部中间 → x=500, y=990。",
            PropertyList({
                Property("x", kPropertyTypeInteger, 0, 1000),
                Property("y", kPropertyTypeInteger, 0, 1000),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto x = properties["x"].value<int>();
                auto y = properties["y"].value<int>();
                auto& portal = PortalClient::GetInstance();
                if (portal.Configured()) {
                    int sw = 0, sh = 0;
                    PortalGetScreenSize(sw, sh);
                    int px = x * sw / 1000, py = y * sh / 1000;
                    bool ok = portal.Tap(px, py);
                    ESP_LOGI(TAG, "[动作] tap 走 Portal 归一化(%d,%d)→像素(%d,%d) → %s",
                             x, y, px, py, ok ? "OK" : "失败");
                    return ok ? Ok("已点击 (" + std::to_string(x) + "," + std::to_string(y) + ")")
                              : std::string("{\"success\": false, \"message\": \"Portal 点击失败，请检查手机上的 Portal 服务\"}");
                }
                bool ok = Hid().TouchTap(x, y);
                return ok ? Ok("已在 (" + std::to_string(x) + "," + std::to_string(y) + ") 点击")
                          : std::string("{\"success\": false, \"message\": \"蓝牙 HID 发送失败：可能未连接，或该手机不支持蓝牙绝对坐标触摸。建议配置 Portal\"}");
            });

        mcp.AddTool(
            "self.phone.double_tap",
            "在屏幕指定位置快速点击两次（如双击放大视频/图片）。坐标规则同 tap。",
            PropertyList({
                Property("x", kPropertyTypeInteger, 0, 1000),
                Property("y", kPropertyTypeInteger, 0, 1000),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto x = properties["x"].value<int>();
                auto y = properties["y"].value<int>();
                auto& hid = Hid();
                bool ok = hid.TouchTap(x, y, 40);
                vTaskDelay(pdMS_TO_TICKS(80));
                ok = ok && hid.TouchTap(x, y, 40);
                return ok ? Ok("已双击") : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.long_press",
            "在屏幕指定位置长按（弹出菜单、拖拽入口等）。坐标规则同 tap。",
            PropertyList({
                Property("x", kPropertyTypeInteger, 0, 1000),
                Property("y", kPropertyTypeInteger, 0, 1000),
                Property("ms", kPropertyTypeInteger, 800, 200, 5000),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto x = properties["x"].value<int>();
                auto y = properties["y"].value<int>();
                auto ms = properties["ms"].value<int>();
                bool ok = Hid().TouchTap(x, y, ms);
                return ok ? Ok("已长按" + std::to_string(ms) + "毫秒") : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.swipe",
            "在屏幕上从一点滑动到另一点（翻页、滑动解锁、拖动进度条等）。坐标规则同 tap。duration 是滑动总时长（毫秒），快滑(如翻页)用120，慢滑用600。",
            PropertyList({
                Property("x1", kPropertyTypeInteger, 0, 1000),
                Property("y1", kPropertyTypeInteger, 0, 1000),
                Property("x2", kPropertyTypeInteger, 0, 1000),
                Property("y2", kPropertyTypeInteger, 0, 1000),
                Property("duration", kPropertyTypeInteger, 300, 80, 2000),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto x1 = properties["x1"].value<int>();
                auto y1 = properties["y1"].value<int>();
                auto x2 = properties["x2"].value<int>();
                auto y2 = properties["y2"].value<int>();
                auto duration = properties["duration"].value<int>();
                auto& portal = PortalClient::GetInstance();
                if (portal.Configured()) {
                    int sw = 0, sh = 0;
                    PortalGetScreenSize(sw, sh);
                    bool ok = portal.Swipe(x1 * sw / 1000, y1 * sh / 1000,
                                           x2 * sw / 1000, y2 * sh / 1000, duration);
                    ESP_LOGI(TAG, "[动作] swipe 走 Portal → %s", ok ? "OK" : "失败");
                    return ok ? Ok("已滑动")
                              : std::string("{\"success\": false, \"message\": \"Portal 滑动失败\"}");
                }
                bool ok = Hid().TouchSwipe(x1, y1, x2, y2, duration);
                return ok ? Ok("已滑动")
                          : std::string("{\"success\": false, \"message\": \"蓝牙 HID 发送失败，建议配置 Portal\"}");
            });

        mcp.AddTool(
            "self.phone.scroll",
            "滚动当前页面。direction 可选 up(内容向上滚,看下面的内容)/down(回看上面)/left(下一页)/right(上一页)。amount 是滚动幅度 1-10，一般用 3。",
            PropertyList({
                Property("direction", kPropertyTypeString),
                Property("amount", kPropertyTypeInteger, 3, 1, 10),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto dir = properties["direction"].value<std::string>();
                auto amount = properties["amount"].value<int>();
                int d = 100 * amount;  // 1000 坐标系里的滑动距离
                auto& hid = Hid();
                bool ok;
                if (dir == "up")       ok = hid.TouchSwipe(500, 500 + d, 500, 500 - d, 350);
                else if (dir == "down")   ok = hid.TouchSwipe(500, 500 - d, 500, 500 + d, 350);
                else if (dir == "left")   ok = hid.TouchSwipe(500 + d, 500, 500 - d, 500, 350);
                else if (dir == "right")  ok = hid.TouchSwipe(500 - d, 500, 500 + d, 500, 350);
                else return std::string("{\"success\": false, \"message\": \"方向只能是 up/down/left/right\"}");
                return ok ? Ok("已向" + dir + "滚动") : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.type",
            "通过 USB 键盘向当前焦点输入框输入文本。只支持英文、数字和常见符号，不支持中文（中文请用手机的语音输入或剪贴板）。\n"
            "输入前先用 tap 点击输入框让光标聚焦。文本中的 \\n 表示回车。",
            PropertyList({
                Property("text", kPropertyTypeString).SetMaxLength(200),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto text = properties["text"].value<std::string>();
                bool ok = Hid().TypeText(text);
                return ok ? Ok("已输入" + std::to_string(text.size()) + "个字符") : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.key",
            "按一个按键。key 可选：back(返回,安卓) home(回主屏,安卓) enter(回车/确认) space(空格) tab esc delete(向前删) backspace(向后删) up/down/left/right(方向键) pageup pagedown home_key end_key volume_up volume_down mute play_pause。\n"
            "安卓上：back=返回上一级，home=回桌面。iPhone 上回主屏请用 home_ios。",
            PropertyList({
                Property("key", kPropertyTypeString),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto key = properties["key"].value<std::string>();
                auto& hid = Hid();
                bool ok = false;
                if      (key == "back")      ok = hid.PressKey(HID_KEY_ESC);
                else if (key == "home")      ok = hid.PressKey(HID_KEY_HOME);
                else if (key == "enter")     ok = hid.PressKey(HID_KEY_RETURN);
                else if (key == "space")     ok = hid.PressKey(HID_KEY_SPACE);
                else if (key == "tab")       ok = hid.PressKey(HID_KEY_TAB);
                else if (key == "esc")       ok = hid.PressKey(HID_KEY_ESC);
                else if (key == "delete")    ok = hid.PressKey(HID_KEY_DELETE);
                else if (key == "backspace") ok = hid.PressKey(HID_KEY_BACKSPACE);
                else if (key == "up")        ok = hid.PressKey(HID_KEY_UP);
                else if (key == "down")      ok = hid.PressKey(HID_KEY_DOWN);
                else if (key == "left")      ok = hid.PressKey(HID_KEY_LEFT);
                else if (key == "right")     ok = hid.PressKey(HID_KEY_RIGHT);
                else if (key == "pageup")    ok = hid.PressKey(HID_KEY_PAGE_UP);
                else if (key == "pagedown")  ok = hid.PressKey(HID_KEY_PAGE_DOWN);
                else if (key == "home_key")  ok = hid.PressKey(HID_KEY_HOME);
                else if (key == "end_key")   ok = hid.PressKey(HID_KEY_END);
                else if (key == "volume_up")   ok = hid.ConsumerKey(CONSUMER_VOLUME_UP);
                else if (key == "volume_down") ok = hid.ConsumerKey(CONSUMER_VOLUME_DOWN);
                else if (key == "mute")        ok = hid.ConsumerKey(CONSUMER_MUTE);
                else if (key == "play_pause")  ok = hid.ConsumerKey(CONSUMER_PLAY_PAUSE);
                else return std::string("{\"success\": false, \"message\": \"未知按键: " + key + "\"}");
                return ok ? Ok("已按 " + key) : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.home_ios",
            "让 iPhone 回到主屏幕（发送 Command+H 组合键）。安卓设备请用 key=home。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                bool ok = Hid().PressCombo(MOD_LGUI, 'H');
                return ok ? Ok("已发送 Cmd+H") : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.combo",
            "发送修饰键+按键的组合键（高级用法）。modifier 可选 ctrl/shift/alt/cmd(gui)，key 填单个字符或 F1-F12。",
            PropertyList({
                Property("modifier", kPropertyTypeString),
                Property("key", kPropertyTypeString),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto mod = properties["modifier"].value<std::string>();
                auto key = properties["key"].value<std::string>();
                uint8_t m = 0;
                if (mod == "ctrl") m = MOD_LCTRL;
                else if (mod == "shift") m = MOD_LSHIFT;
                else if (mod == "alt") m = MOD_LALT;
                else if (mod == "cmd" || mod == "gui") m = MOD_LGUI;
                else return std::string("{\"success\": false, \"message\": \"modifier 只能是 ctrl/shift/alt/cmd\"}");
                uint8_t usage = 0;
                if (key.size() == 1) {
                    char c = key[0];
                    if (c >= 'a' && c <= 'z') usage = 0x04 + (c - 'a');
                    else if (c >= 'A' && c <= 'Z') usage = 0x04 + (c - 'A');
                    else if (c >= '1' && c <= '9') usage = 0x1E + (c - '1');
                } else if (key.size() >= 2 && (key[0] == 'F' || key[0] == 'f')) {
                    int fn = atoi(key.c_str() + 1);
                    if (fn >= 1 && fn <= 12) usage = 0x3A + fn - 1;
                }
                if (usage == 0) return std::string("{\"success\": false, \"message\": \"key 只支持字母/数字/F1-F12\"}");
                bool ok = Hid().PressCombo(m, usage);
                return ok ? Ok("已发送组合键") : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.mouse",
            "用鼠标模式操作（光标相对移动）。action 可选 move/click/right_click/dblclick/scroll_up/scroll_down。dx、dy 是移动量（-5000..5000，仅在 move 时有效）。",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("dx", kPropertyTypeInteger, 0, -5000, 5000),
                Property("dy", kPropertyTypeInteger, 0, -5000, 5000),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                auto dx = properties["dx"].value<int>();
                auto dy = properties["dy"].value<int>();
                auto& hid = Hid();
                bool ok = false;
                if      (action == "move")        ok = hid.MouseMove(dx, dy);
                else if (action == "click")       ok = hid.MouseClick(0x01);
                else if (action == "right_click") ok = hid.MouseClick(0x02);
                else if (action == "dblclick") { ok = hid.MouseClick(0x01); vTaskDelay(pdMS_TO_TICKS(60)); ok = ok && hid.MouseClick(0x01); }
                else if (action == "scroll_up")   ok = hid.MouseScroll(3);
                else if (action == "scroll_down") ok = hid.MouseScroll(-3);
                else return std::string("{\"success\": false, \"message\": \"未知 action\"}");
                return ok ? Ok("鼠标操作完成") : std::string("{\"success\": false, \"message\": \"HID 报告发送失败：手机可能未连接或不支持该报告，可以说「诊断手机控制」查看链路状态\"}");
            });

        mcp.AddTool(
            "self.phone.get_status",
            "查询手机控制链路状态：蓝牙 HID 是否连接、Portal 无障碍是否配置、当前走哪条通道。用户问「手机连上了吗」「能控制手机吗」时调用。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& ble = PhoneBleHid::GetInstance();
                auto& portal = PortalClient::GetInstance();
                std::string msg = "蓝牙HID: ";
                msg += ble.IsReady() ? "已启动" : "未启动";
                msg += ble.IsConnected() ? "、手机已连接" : "、等待手机连接";
                msg += "；Portal: ";
                msg += portal.Configured() ? ("已配置 " + portal.Host()) : "未配置";
                msg += "；点击通道: ";
                msg += portal.Configured() ? "Portal 无障碍(可靠)" : "蓝牙绝对坐标(部分安卓不支持)";
                ESP_LOGI(TAG, "[状态] %s", msg.c_str());
                return Ok(msg);
            });

        mcp.AddTool(
            "self.phone.diag",
            "诊断手机控制链路。依次向蓝牙 HID 的 4 个报告各发一个全释放空包（不会在手机上产生可见动作），"
            "用来确认报告映射、连接状态与通知通道是否正常。用户说「诊断手机控制」「为什么控制不了手机」时调用。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& ble = PhoneBleHid::GetInstance();
                static const uint8_t kKbd[8] = {0};
                static const uint8_t kMouse[4] = {0};
                static const uint8_t kConsumer[2] = {0};
                static const uint8_t kTouch[5] = {0};
                bool r1 = ble.Send(1, kKbd, sizeof(kKbd));
                bool r2 = ble.Send(2, kMouse, sizeof(kMouse));
                bool r3 = ble.Send(3, kConsumer, sizeof(kConsumer));
                bool r4 = ble.Send(4, kTouch, sizeof(kTouch));
                std::string msg = "蓝牙HID ready=" + std::to_string(ble.IsReady()) +
                    " connected=" + std::to_string(ble.IsConnected()) +
                    "；报告自检 键盘=" + std::to_string(r1) +
                    " 鼠标=" + std::to_string(r2) +
                    " 多媒体=" + std::to_string(r3) +
                    " 触摸屏=" + std::to_string(r4) +
                    "；Portal=" + std::string(PortalClient::GetInstance().Configured() ? "已配置" : "未配置");
                ESP_LOGI(TAG, "[诊断] %s", msg.c_str());
                return Ok(msg);
            });

        mcp.AddTool(
            "self.phone.nav",
            "通过蓝牙 HID 发送「按键级」操作——安卓 / iPhone / 电脑都可靠支持这条通道。\n"
            "action 可取：home(回主屏) / back(返回) / enter(确认) / tab(下一个焦点) / esc / delete(退格) / "
            "space / up / down / left / right / page_up / page_down / play(播放暂停) / volume_up / volume_down / menu。\n"
            "用户说「按返回」「回桌面」「下一项」「播放」「音量调大」时调用本工具。\n"
            "要按坐标点屏幕用 self.phone.tap，要读屏幕内容用 self.phone.screen。",
            PropertyList({ Property("action", kPropertyTypeString) }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string a = properties["action"].value<std::string>();
                for (auto& c : a) c = (char)tolower((unsigned char)c);
                bool ok = false;
                if (a == "home") ok = Hid().ConsumerKey(0x0223);           // AC Home
                else if (a == "back") ok = Hid().ConsumerKey(0x0224);      // AC Back
                else if (a == "menu") ok = Hid().PressKey(HID_KEY_ESC);
                else if (a == "play") ok = Hid().ConsumerKey(CONSUMER_PLAY_PAUSE);
                else if (a == "volume_up") ok = Hid().ConsumerKey(CONSUMER_VOLUME_UP);
                else if (a == "volume_down") ok = Hid().ConsumerKey(CONSUMER_VOLUME_DOWN);
                else if (a == "enter") ok = Hid().PressKey(HID_KEY_RETURN);
                else if (a == "tab") ok = Hid().PressKey(HID_KEY_TAB);
                else if (a == "esc") ok = Hid().PressKey(HID_KEY_ESC);
                else if (a == "delete") ok = Hid().PressKey(HID_KEY_BACKSPACE);
                else if (a == "space") ok = Hid().PressKey(HID_KEY_SPACE);
                else if (a == "up") ok = Hid().PressKey(HID_KEY_UP);
                else if (a == "down") ok = Hid().PressKey(HID_KEY_DOWN);
                else if (a == "left") ok = Hid().PressKey(HID_KEY_LEFT);
                else if (a == "right") ok = Hid().PressKey(HID_KEY_RIGHT);
                else if (a == "page_up") ok = Hid().PressKey(HID_KEY_PAGE_UP);
                else if (a == "page_down") ok = Hid().PressKey(HID_KEY_PAGE_DOWN);
                else return std::string("{\"success\": false, \"message\": \"不支持的 action\"}");
                ESP_LOGI(TAG, "[动作] nav %s → %s", a.c_str(), ok ? "OK" : "失败");
                return ok ? Ok("已按下 " + a)
                          : std::string("{\"success\": false, \"message\": \"蓝牙 HID 未连接或报告发送失败\"}");
            });
    }
};

// touch rebuild

PhoneHidController::PhoneHidController() {
#if PHONE_HID_USE_USB
    Hid().Init();          // USB + 蓝牙
#else
    ESP_LOGI(TAG, "屏幕模式：GPIO19/20 让给 LCD，仅启用蓝牙 HID");
    PhoneBleHid::GetInstance().Init();
#endif
    RegisterTools();
    PortalClient::GetInstance().Load();
    RegisterPortalTools();
    ESP_LOGI(TAG, "手机控制 MCP 工具注册完成");
    StartMonitorTask();
}

// ---------- 自测与状态监控 ----------
//
// 一键自测：打字 + 鼠标移动，接电脑/手机都能直观验证。
// 触发方式：串口命令 `selftest`，或 MCP 工具 self.phone.diag。
// 注意：不要绑定 BOOT 键，那是小智「开始/停止聆听」的原生按键。
// 监控线程同时跟踪 HID 链路状态变化并打日志。

static void SelfTest() {
    auto& hid = Hid();
    ESP_LOGI(TAG, "[自测] 蓝牙 ready=%d connected=%d，USB挂载=%d，Portal配置=%d",
             PhoneBleHid::GetInstance().IsReady(), PhoneBleHid::GetInstance().IsConnected(),
             PhoneHid::GetInstance().IsMounted(), PortalClient::GetInstance().Configured());
    // 不再因为 IsMounted() 为假就直接放弃：连接事件可能丢失，
    // 现在强行尝试发送，由底层返回真实失败原因并打日志。
    ESP_LOGI(TAG, "[自测] 发送 HID_TEST_OK_123 + 回车 + 鼠标移动（请在手机/电脑上打开输入框观察）");
    bool ok = hid.TypeText("HID_TEST_OK_123\n");
    ok = hid.MouseMove(200, 100) && ok;
    ESP_LOGI(TAG, "[自测] 结果: %s", ok ? "全部报告发送成功 ✓（看输入框有没有字）"
                                        : "有 report 发送失败 ✗（看上面的失败原因）");
}

// ---------- 串口诊断控制台 ----------
//
// 板上的 USB-串口芯片走 UART0，是独立于 GPIO19/20（被 LCD 占用）的有线通道。
// 于是可以在「屏幕正常亮 + 不装 App + 不配蓝牙」的前提下，从电脑直接下发
// 控制命令并观察每一个 HID 报告的原始字节，做完整的端到端实测。
// Mac 侧配套 tools/serial_hid_bridge.py 会把 [[HID:id:hex]] 解析成真实键鼠事件。

static void SendNamedKey(const std::string& a, bool& ok) {
    auto& hid = Hid();
    if      (a == "home")        ok = hid.ConsumerKey(0x0223);
    else if (a == "back")        ok = hid.ConsumerKey(0x0224);
    else if (a == "play")        ok = hid.ConsumerKey(CONSUMER_PLAY_PAUSE);
    else if (a == "volume_up")   ok = hid.ConsumerKey(CONSUMER_VOLUME_UP);
    else if (a == "volume_down") ok = hid.ConsumerKey(CONSUMER_VOLUME_DOWN);
    else if (a == "mute")        ok = hid.ConsumerKey(CONSUMER_MUTE);
    else if (a == "enter")       ok = hid.PressKey(HID_KEY_RETURN);
    else if (a == "tab")         ok = hid.PressKey(HID_KEY_TAB);
    else if (a == "esc")         ok = hid.PressKey(HID_KEY_ESC);
    else if (a == "delete")      ok = hid.PressKey(HID_KEY_BACKSPACE);
    else if (a == "space")       ok = hid.PressKey(HID_KEY_SPACE);
    else if (a == "up")          ok = hid.PressKey(HID_KEY_UP);
    else if (a == "down")        ok = hid.PressKey(HID_KEY_DOWN);
    else if (a == "left")        ok = hid.PressKey(HID_KEY_LEFT);
    else if (a == "right")       ok = hid.PressKey(HID_KEY_RIGHT);
    else ok = false;
}

static void RunCommand(const std::string& raw) {
    if (raw.empty()) return;
    size_t sp = raw.find(' ');
    std::string cmd = sp == std::string::npos ? raw : raw.substr(0, sp);
    std::string args = sp == std::string::npos ? "" : raw.substr(sp + 1);
    for (auto& c : cmd) c = (char)tolower((unsigned char)c);
    while (!args.empty() && (args.back() == ' ' || args.back() == '\t')) args.pop_back();
    ESP_LOGI(TAG, "[CMD] > %s", raw.c_str());

    std::string res;
    auto& hid = Hid();
    auto& ble = PhoneBleHid::GetInstance();
    auto& portal = PortalClient::GetInstance();

    if (cmd == "help" || cmd == "?") {
        res = "命令: status | state | chat | bridge on|off | type <英文> | "
              "nav <home/back/enter/tab/up/...> | tap <x> <y> | swipe <x1> <y1> <x2> <y2> | "
              "mouse <dx> <dy> | diag | selftest | portal <IP> <token> | screen | "
              "click <控件文字> | input <中文> | app <包名> | ptap <px> <py> | pswipe ... | pkey <code>";
    } else if (cmd == "state") {
        static const char* kNames[] = {
            "unknown", "starting", "wifi_configuring", "idle", "connecting",
            "listening", "speaking", "notifying", "upgrading", "activating",
            "audio_testing", "fatal_error"};
        int s = (int)Application::GetInstance().GetDeviceState();
        res = "设备状态=" + std::to_string(s) + " (" +
              (s >= 0 && s < (int)(sizeof(kNames) / sizeof(kNames[0])) ? kNames[s] : "?") +
              ")；唤醒词/聆听就绪请用 chat 命令模拟按键验证";
    } else if (cmd == "chat") {
        // 软件模拟 BOOT 键短按：与板级 InitializeButtons() 的 OnClick 回调完全同一个入口。
        // 用于在没有手的情况下验证「按键 → 开始聆听 → 音频上行」链路。
        Application::GetInstance().ToggleChatState();
        res = "已调用 ToggleChatState()（等价于按一次 BOOT 键），请看状态机日志";
    } else if (cmd == "status") {
        res = "蓝牙 ready=" + std::to_string(ble.IsReady()) +
              " connected=" + std::to_string(ble.IsConnected()) +
              "；USB挂载=" + std::to_string(PhoneHid::GetInstance().IsMounted()) +
              "；Portal=" + (portal.Configured() ? portal.Host() : std::string("未配置")) +
              "；串口HID回显=已启用；串口桥=" + (ble.SerialBridgeEnabled() ? "ON" : "OFF");
    } else if (cmd == "bridge") {
        bool on = (args == "on" || args == "1" || args == "true");
        ble.SetSerialBridge(on);
        res = std::string("串口桥已") + (on ? "打开：报告经 UART0 送达电脑，蓝牙未连也算成功"
                                           : "关闭：恢复严格的蓝牙连接判断");
    } else if (cmd == "type") {
        res = hid.TypeText(args) ? "打字成功（看 [[HID:...]] 回显）" : "打字失败（看上方 [发送] 日志）";
    } else if (cmd == "nav") {
        bool ok = false;
        SendNamedKey(args, ok);
        res = ok ? "按键成功" : "按键失败或未知按键";
    } else if (cmd == "tap") {
        int x = 0, y = 0;
        if (sscanf(args.c_str(), "%d %d", &x, &y) != 2) res = "用法: tap <0-1000> <0-1000>";
        else res = hid.TouchTap(x, y) ? "触摸点击报告已生成" : "点击失败";
    } else if (cmd == "swipe") {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (sscanf(args.c_str(), "%d %d %d %d", &x1, &y1, &x2, &y2) != 4)
            res = "用法: swipe <x1> <y1> <x2> <y2>";
        else res = hid.TouchSwipe(x1, y1, x2, y2, 400) ? "滑动报告已生成" : "滑动失败";
    } else if (cmd == "mouse") {
        int dx = 0, dy = 0;
        if (sscanf(args.c_str(), "%d %d", &dx, &dy) != 2) res = "用法: mouse <dx> <dy>";
        else res = hid.MouseMove(dx, dy) ? "鼠标报告已生成" : "鼠标失败";
    } else if (cmd == "diag") {
        static const uint8_t zero[8] = {0};
        bool r1 = ble.Send(1, zero, 8), r2 = ble.Send(2, zero, 4);
        bool r3 = ble.Send(3, zero, 2), r4 = ble.Send(4, zero, 5);
        res = "报告自检 键盘=" + std::to_string(r1) + " 鼠标=" + std::to_string(r2) +
              " 多媒体=" + std::to_string(r3) + " 触摸屏=" + std::to_string(r4) +
              "（未连接时发送会失败，但 [[HID:...]] 回显仍能验证编码）";
    } else if (cmd == "selftest") {
        SelfTest();
        res = "自测已执行（见上方 [自测] 日志）";
    } else {
        res = PortalConsoleCommand(cmd, args);
    }
    ESP_LOGI(TAG, "[CMD] < %s", res.c_str());
}

void PhoneHidController::StartMonitorTask() {
    // 【重要】绝对不要碰 GPIO0！
    // 板级 compact_wifi_board_s3cam.cc 的 Button boot_button_(GPIO_NUM_0) 已经在
    // InitializeButtons() 里配置了 GPIO0 中断并绑定 OnClick -> Application::ToggleChatState()
    // （短按开始/停止聆听、启动时进配网）。这里曾经为了做「按键自测 HID」又调用了一次
    // gpio_config(&io)，结果把 Button 的中断类型清成 DISABLE，短按再也进不了聆听状态 ——
    // 这正是「按小智没反应、之前能听我说话」的根因。
    // 自测请改用串口命令 `selftest`，不要占用物理按键。
    //
    // UART0 已被日志占用 TX，这里只接管 RX。串口控制台与监控共用一个任务，
    // 省下 4KB 内部 SRAM —— 内部 RAM 很紧，TLS 握手需要连续 DMA 块，能省则省。
    esp_err_t e = uart_driver_install(UART_NUM_0, 512, 0, 0, nullptr, 0);
    ESP_LOGI(TAG, "[控制台] UART0 驱动: %s；可直接从串口发命令，输入 help 查看",
             esp_err_to_name(e));

    xTaskCreate([](void* arg) {
        bool last_mounted = Hid().IsMounted();
        std::string cmd_line;
        uint8_t rx[64];
        while (true) {
            // 1) 串口命令（非阻塞）
            int n = uart_read_bytes(UART_NUM_0, rx, sizeof(rx), 0);
            for (int i = 0; i < n; i++) {
                char c = (char)rx[i];
                if (c == '\r' || c == 0) continue;
                if (c == '\n') {
                    if (!cmd_line.empty()) RunCommand(cmd_line);
                    cmd_line.clear();
                } else if (cmd_line.size() < 200) {
                    cmd_line.push_back(c);
                }
            }
            // 2) HID 链路状态变化
            bool mounted = Hid().IsMounted();
            if (mounted != last_mounted) {
                ESP_LOGI(TAG, "HID 链路 %s (蓝牙/USB 主机%s)",
                         mounted ? "已连接 ✓" : "已断开",
                         mounted ? "完成连接" : "断开");
                last_mounted = mounted;
            }
            // 3) 不再轮询 GPIO0：物理按键完全交还给板级 Button 驱动。
            //    自测统一走串口 `selftest` 命令 / MCP self.phone.diag 工具。
            vTaskDelay(pdMS_TO_TICKS(60));
        }
    }, "hid_monitor", 4096, this, 3, nullptr);
}
