#pragma once

#include <string>

// 注册 Portal（安卓无障碍通道）相关的 MCP 工具
void RegisterPortalTools();

// 最近一次读屏得到的屏幕像素尺寸；没读过时返回 1080x2400。
// phone_hid_controller 的 tap/swipe 需要它把 0-1000 归一化坐标换成像素坐标。
void PortalSetScreenSize(int w, int h);
void PortalGetScreenSize(int& w, int& h);

// 串口诊断控制台用：直接执行一条 Portal 命令并返回结果文本。
// 支持 portal / screen / click / input / app / ptap / pswipe / pkey。
std::string PortalConsoleCommand(const std::string& cmd, const std::string& args);
