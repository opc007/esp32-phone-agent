#pragma once

// S31 Korvo-1 板不需要「ESP32 直连手机」链路（见 phone_bridge_stub.cc）。
// 这里只声明 stub 实现的三个函数，供从 bread 板复制来的 mac_control_tools.cc 编译。
// PhoneBridgeActive() 恒为 false，因此所有控制动作都走 Portal HTTP -> Mac。
#include <string>
#include <vector>

bool PhoneBridgeActive();
std::string PhoneBridgeExecute(const std::string& path, const std::string& body);
std::string PhoneBridgeConsoleCommand(const std::string& cmd,
                                      const std::vector<std::string>& args);
