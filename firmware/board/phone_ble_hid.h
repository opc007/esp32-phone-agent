/*
 * PhoneBleHid —— 蓝牙（BLE HOGP）版手机控制
 *
 * 与 PhoneHid（USB）接口保持一致，内部走 esp_hidd + NimBLE。
 * 一个 HID 报告映射里包含 4 个 report：键盘(1)/鼠标(2)/多媒体(3)/触摸屏(4)。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

class PhoneBleHid {
public:
    static PhoneBleHid& GetInstance();

    // 启动 BLE HID（幂等，内部起任务，不阻塞）
    void Init();
    // 手机/电脑是否已连接（蓝牙配对并连上）
    bool IsConnected();
    // 蓝牙是否已就绪（可被搜索/连接）
    bool IsReady();

    // 串口桥模式：HID 报告都会回显到 UART0，若电脑侧跑着
    // tools/serial_hid_bridge.py，即使蓝牙没有配对也算「已送达」。
    // 默认关闭，不改变正常的连接状态判断；用串口命令 bridge on 打开。
    void SetSerialBridge(bool on) { serial_bridge_ = on; }
    bool SerialBridgeEnabled() const { return serial_bridge_; }

    bool Send(uint8_t report_id, const uint8_t* data, size_t len);

    bool TypeText(const std::string& text, int per_char_delay_ms = 15);
    bool PressKey(uint8_t usage, int hold_ms = 30);
    bool PressCombo(uint8_t modifier, uint8_t usage, int hold_ms = 40);

    bool MouseMove(int dx, int dy);
    bool MouseClick(uint8_t button = 0x01);
    bool MouseScroll(int8_t wheel);

    bool TouchTap(int x, int y, int hold_ms = 60);   // x/y: 0..1000
    bool TouchSwipe(int x1, int y1, int x2, int y2, int duration_ms = 400);

    bool ConsumerKey(uint16_t usage, int hold_ms = 60);

private:
    PhoneBleHid() = default;
    void StartTask();
    static void Ready(bool connected);
    bool inited_ = false;
    bool serial_bridge_ = false;
};
