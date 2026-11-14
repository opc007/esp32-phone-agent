#include "voice_selftest.h"

#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/uart.h>

#include <cctype>
#include <string>
#include <vector>

#include "application.h"
#include "device_state.h"
#if MAC_CONTROL_ENABLE
#include "mac_control_tools.h"
#endif
#include "phone_bridge.h"

#define TAG "VoiceSelfTest"

static const char* StateName(int s) {
    static const char* kNames[] = {
        "unknown", "starting", "wifi_configuring", "idle", "connecting",
        "listening", "speaking", "notifying", "upgrading", "activating",
        "audio_testing", "fatal_error"};
    return (s >= 0 && s < (int)(sizeof(kNames) / sizeof(kNames[0]))) ? kNames[s] : "?";
}

static std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void Run(const std::string& raw) {
    auto parts = Split(raw);
    if (parts.empty()) return;
    std::string cmd = parts[0];
    for (auto& c : cmd) c = (char)tolower((unsigned char)c);
    // 参数保留原样：应用名、中文文本区分大小写/编码
    std::vector<std::string> args(parts.begin() + 1, parts.end());
    ESP_LOGI(TAG, "[CMD] > %s", raw.c_str());

    if (cmd == "help" || cmd == "?") {
        ESP_LOGI(TAG, "[CMD] < 命令: state | chat | say <模拟说一句话> | macapps(读真实应用列表) | "
                      "macapp <应用名> | macplay <歌曲名> | macsong <点歌整句> | "
                      "macbrowser <浏览器整句：打开浏览器/搜索/按回车> | "
                      "macphone <手机整句：打开手机上的X/查价格/回复> | "
                      "phone <action> [query] (apps/state/open/bootstrap...) | "
                      "macmsg <发消息整句> | "
                      "macsend <应用> <联系人> <内容> | macscreen(读真实屏幕) | "
                      "mactest(模拟云端调用工具) | mac(测连通) | macstatus | "
                      "macclick <x> <y> | macinput <文字> | macset <IP:8080> [token] | "
                      "phonestatus | phonebind <手机IP>");
    } else if (cmd == "state") {
        int s = (int)Application::GetInstance().GetDeviceState();
        ESP_LOGI(TAG, "[CMD] < 设备状态=%d (%s)", s, StateName(s));
    } else if (cmd == "chat") {
        // 与板级 InitializeButtons() 的 OnClick 完全同一个入口
        Application::GetInstance().ToggleChatState();
        ESP_LOGI(TAG, "[CMD] < 已调用 ToggleChatState()，看下面状态机是否进入 listening");
#if MAC_CONTROL_ENABLE
    } else {
        std::string reply = MacControlConsoleCommand(cmd, args);
        if (reply.empty()) reply = PhoneBridgeConsoleCommand(cmd, args);
        if (!reply.empty()) {
            ESP_LOGI(TAG, "[CMD] < %s", reply.c_str());
        } else {
            ESP_LOGI(TAG, "[CMD] < 未知命令，输入 help");
        }
    }
#else
    } else {
        ESP_LOGI(TAG, "[CMD] < 未知命令，输入 help");
    }
#endif
}

void StartVoiceSelfTest() {
    // IDF console 可能已经装过 UART0 驱动；装过就直接复用，不装才创建。
    esp_err_t e = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
    ESP_LOGI(TAG, "[自测] UART0: %s；串口敲 help / state / chat", esp_err_to_name(e));

    // 4KB 栈：HTTP 动作已移到独立的 mac_http 任务（8KB），这里只做命令解析。
    xTaskCreate([](void*) {
        std::string line;
        uint8_t rx[32];
        while (true) {
            int n = uart_read_bytes(UART_NUM_0, rx, sizeof(rx), 0);
            for (int i = 0; i < n; i++) {
                char c = (char)rx[i];
                if (c == '\r' || c == 0) continue;
                if (c == '\n') {
                    Run(line);
                    line.clear();
                } else if (line.size() < 192) {
                    line.push_back(c);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }, "voice_selftest", 4096, nullptr, 2, nullptr);
}
