/*
 * PhoneHid —— 手机/电脑 USB HID 控制封装
 *
 * 底层是复合 HID 设备：键盘 + 鼠标 + 绝对坐标触摸屏 + 多媒体键。
 * 上层通过 MCP 工具暴露给小智的大模型调用。
 */
#pragma once

#include <string>

class PhoneHid {
public:
    static PhoneHid& GetInstance();

    // 初始化 TinyUSB（幂等）
    void Init();

    // USB 是否已由主机枚举（手机/电脑识别出 HID）
    bool IsMounted();

    // ---- 键盘 ----
    // 输入 ASCII 文本（\n 回车 \t Tab \b 退格）。仅支持英文/数字/符号。
    bool TypeText(const std::string& text, int per_char_delay_ms = 12);
    // 单个键（HID usage），按下并释放
    bool PressKey(uint8_t usage, int hold_ms = 30);
    // 修饰键组合：modifier 为 HID modifier bit，如 KEYBOARD_MODIFIER_LEFTGUI
    bool PressCombo(uint8_t modifier, uint8_t usage, int hold_ms = 40);

    // ---- 鼠标 ----
    bool MouseMove(int dx, int dy);
    bool MouseClick(uint8_t button = 0x01);   // bit0 左键
    bool MouseScroll(int8_t wheel);           // 正=向上

    // ---- 触摸屏（绝对坐标） ----
    // x/y 取值 0..1000，内部映射到 0..4095
    bool TouchTap(int x, int y, int hold_ms = 60);
    // 按住从 (x1,y1) 滑到 (x2,y2)，总时长 duration_ms
    bool TouchSwipe(int x1, int y1, int x2, int y2, int duration_ms = 400);

    // ---- 多媒体键 ----
    bool ConsumerKey(uint16_t usage, int hold_ms = 60);

private:
    PhoneHid() = default;
    bool SendReport(uint8_t report_id, const void* data, uint16_t len);
    bool SendKeyboard(uint8_t modifier, const uint8_t key[6]);
    void DelayMs(int ms);

    bool inited_ = false;
};
