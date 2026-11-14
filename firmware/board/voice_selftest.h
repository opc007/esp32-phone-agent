#pragma once

// 极简串口自测通道：只依赖 UART0 + Application，不依赖蓝牙/USB HID/Portal。
// 用来在没有手按 BOOT 键的情况下验证「唤醒 → 开始聆听 → 音频上行」链路。
// 命令：help / state / chat
void StartVoiceSelfTest();
