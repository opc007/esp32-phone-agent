#pragma once

#include <string>
#include <vector>

// ESP32 自托管手机接入：内置无障碍 APK 下载页、自动发现手机、以及固件内的
// 手机控制协议桥。启动后不需要电脑、ADB、root、Termux 或手工填写 IP。
void StartPhoneBridge();

// 是否已经发现并连上手机上的无障碍服务（:7333）。
bool PhoneBridgeActive();

// 执行一条 Portal 风格的工具请求（path + JSON body），返回中文结果文本。
// mac_control_tools.cc 在手机直连模式下直接调用它，避免 HTTP 自环。
std::string PhoneBridgeExecute(const std::string& path, const std::string& body);

// 串口/外部调试用：手动指定或查询手机 IP。
std::string PhoneBridgeConsoleCommand(const std::string& cmd,
                                      const std::vector<std::string>& args);
