/*
 * phone_bridge_stub.cc —— ESP32-S31 板上不需要「ESP32 直连手机」链路。
 *
 * mac_control_tools.cc 里保留了手机直连的分支（BridgeExec / PhoneBridgeActive），
 * 这里提供最小桩实现：让 PhoneBridgeActive() 永远返回 false，
 * 于是所有控制动作都走 Portal HTTP -> Mac，不需要编译庞大的 phone_bridge.cc
 * （它依赖 httpd + 内嵌 APK，S31 板上用不到）。
 */
#include <string>
#include <vector>

bool PhoneBridgeActive() {
    return false;
}

std::string PhoneBridgeExecute(const std::string& path, const std::string& body) {
    (void)path;
    (void)body;
    return "当前固件未启用 ESP32 直连手机模式";
}

std::string PhoneBridgeConsoleCommand(const std::string& cmd,
                                      const std::vector<std::string>& args) {
    (void)cmd;
    (void)args;
    return "未启用";
}
