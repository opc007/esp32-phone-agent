/*
 * qibao_console.cc —— 七宝妹妹的串口控制台（ESP32-S31-Korvo-1）
 *
 * 这块板原来没有串口命令台，只能看日志。这里补一个极简的 UART0 行读取任务：
 * 敲一行回车就执行一条命令，不用重新烧固件就能调试。
 *
 *   help                     命令列表
 *   state                    当前设备状态
 *   qrotate                  画面翻转 180°（和板载长按 MODE、屏幕按钮等效）
 *   qpose                    站姿 / 坐姿 手动切换
 *   qstatus                  人脸门禁 + 屏幕方向 状态
 *   wifiset <ssid> <密码>    写入 WiFi 凭据并重启（重启后自动连上）
 *   wifilist                 看已保存的 WiFi
 *   其余命令转给 mac_control_tools（macphone / macapps / macscreen ...）
 */
#include "qibao_console.h"

#include <cctype>
#include <string>
#include <vector>

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "device_state.h"
#if MAC_CONTROL_ENABLE
#include "mac_control_tools.h"
#include "qibao_display.h"
#include "qibao_persona.h"
#include "qibao_screen.h"
#endif
#include "ssid_manager.h"

#define TAG "QibaoConsole"

namespace {

const char* StateName(int s) {
    static const char* kNames[] = {
        "unknown", "starting", "wifi_configuring", "idle", "connecting",
        "listening", "speaking", "notifying", "upgrading", "activating",
        "audio_testing", "fatal_error"};
    return (s >= 0 && s < (int)(sizeof(kNames) / sizeof(kNames[0]))) ? kNames[s] : "?";
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Join(const std::vector<std::string>& args, size_t from) {
    std::string s;
    for (size_t i = from; i < args.size(); ++i) {
        if (i > from) s += " ";
        s += args[i];
    }
    return s;
}

// 把二进制流编成 base64 并按行打印（每行 240 字符）。
// 电脑端按 [SHOT] 行拼起来就能还原成 JPEG。
void PrintBase64(const std::string& data) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string line;
    line.reserve(256);
    size_t i = 0;
    while (i < data.size()) {
        uint32_t v = 0;
        int n = 0;
        for (int k = 0; k < 3; ++k) {
            v <<= 8;
            if (i + k < data.size()) {
                v |= (uint8_t)data[i + k];
                n = k + 1;
            }
        }
        i += n;
        char quad[4];
        quad[0] = kTable[(v >> 18) & 0x3F];
        quad[1] = kTable[(v >> 12) & 0x3F];
        quad[2] = (n > 1) ? kTable[(v >> 6) & 0x3F] : '=';
        quad[3] = (n > 2) ? kTable[v & 0x3F] : '=';
        for (int k = 0; k < 4; ++k) {
            line.push_back(quad[k]);
            if (line.size() >= 240) {
                ESP_LOGI(TAG, "[SHOT] %s", line.c_str());
                line.clear();
            }
        }
    }
    if (!line.empty()) {
        ESP_LOGI(TAG, "[SHOT] %s", line.c_str());
    }
}

void Run(const std::string& raw) {
    auto parts = Split(raw);
    if (parts.empty()) return;
    std::string cmd = parts[0];
    for (auto& c : cmd) c = (char)tolower((unsigned)c);
    std::vector<std::string> args(parts.begin() + 1, parts.end());
    ESP_LOGI(TAG, "[CMD] > %s", raw.c_str());

    if (cmd == "help" || cmd == "?") {
        ESP_LOGI(TAG, "[CMD] < help | state | qrotate(翻转画面) | qpose(换姿势) | qstatus | "
                      "wifiset <ssid> <密码> | wifilist | macphone <整句> | macapps | macscreen");
        return;
    }
    if (cmd == "state") {
        int s = (int)Application::GetInstance().GetDeviceState();
        ESP_LOGI(TAG, "[CMD] < 设备状态=%d (%s)", s, StateName(s));
        return;
    }
#if MAC_CONTROL_ENABLE
    if (cmd == "qrotate") {
        ESP_LOGI(TAG, "[CMD] < %s", qibao::ToggleRotation().c_str());
        return;
    }
    if (cmd == "qpose") {
        auto* disp = QibaoLcdDisplay::Instance();
        if (disp == nullptr) {
            ESP_LOGI(TAG, "[CMD] < 显示还没初始化，等一会儿再试");
            return;
        }
        disp->SwitchPose();
        ESP_LOGI(TAG, "[CMD] < 已换成%s", disp->CurrentFrame() == 0 ? "站姿" : "坐姿");
        return;
    }
    if (cmd == "qstatus") {
        std::string s = qibao::StatusJson();
        s += "\n屏幕方向: ";
        s += qibao::RotationName();
        ESP_LOGI(TAG, "[CMD] < %s", s.c_str());
        return;
    }
    if (cmd == "qprobe") {
        // 采样当前送显缓冲，验证旋转是否真的作用到了送显内容
        qibao::ProbeFrame();
        return;
    }
    if (cmd == "qdiag") {
        // 诊断：打印 LVGL 变换状态 + 放洋红标记读四角像素
        qibao::DumpTransformState();
        qibao::RunMarkerTest();
        return;
    }
    if (cmd == "qshot") {
        // 抓一张屏幕快照，base64 分段打印，方便在电脑上还原成图片核对画面
        auto* disp = QibaoLcdDisplay::Instance();
        if (disp == nullptr) {
            ESP_LOGI(TAG, "[CMD] < 显示还没初始化，等一会儿再试");
            return;
        }
        std::string jpeg;
        if (!disp->SnapshotToJpeg(jpeg, 70)) {
            ESP_LOGI(TAG, "[CMD] < 截屏失败");
            return;
        }
        ESP_LOGI(TAG, "[SHOT] BEGIN %u", (unsigned)jpeg.size());
        PrintBase64(jpeg);
        ESP_LOGI(TAG, "[SHOT] END");
        return;
    }
#endif
    if (cmd == "wifiset") {
        if (args.size() < 2) {
            ESP_LOGI(TAG, "[CMD] < 用法: wifiset <ssid> <密码>");
            return;
        }
        std::string ssid = args[0];
        std::string password = Join(args, 1);
        SsidManager::GetInstance().AddSsid(ssid, password);
        ESP_LOGI(TAG, "[CMD] < 已保存 WiFi [%s]，3 秒后重启联网", ssid.c_str());
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        return;
    }
    if (cmd == "wifilist") {
        auto& list = SsidManager::GetInstance().GetSsidList();
        if (list.empty()) {
            ESP_LOGI(TAG, "[CMD] < 还没保存任何 WiFi");
            return;
        }
        for (size_t i = 0; i < list.size(); ++i) {
            ESP_LOGI(TAG, "[CMD] < [%d] %s (密码 %d 位)", (int)i, list[i].ssid.c_str(),
                     (int)list[i].password.size());
        }
        return;
    }
#if MAC_CONTROL_ENABLE
    {
        std::string reply = MacControlConsoleCommand(cmd, args);
        if (!reply.empty()) {
            ESP_LOGI(TAG, "[CMD] < %s", reply.c_str());
            return;
        }
    }
#endif
    ESP_LOGI(TAG, "[CMD] < 未知命令，输入 help");
}

}  // namespace

void StartQibaoConsole() {
    // IDF console 可能已经装过 UART0 驱动；装过就直接复用，不装才创建。
    esp_err_t e = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
    ESP_LOGI(TAG, "UART0: %s；串口敲 help / qrotate / qpose / wifiset <ssid> <密码>",
             esp_err_to_name(e));

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
    }, "qibao_console", 4096, nullptr, 2, nullptr);
}
