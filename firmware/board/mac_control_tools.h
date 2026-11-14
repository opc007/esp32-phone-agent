#pragma once

#include <string>
#include <vector>

// 设备端 MCP 工具：通过小智原生语音通道上报，云端大模型可直接调用，
// 不需要小智后台的「MCP 接入点」，也不需要视觉模型 API Key。
// 动作经 WiFi 发到这台 Mac 上常驻的 computer_portal.py (0.0.0.0:8080)。
void StartMacControlTools();

// 串口自测命令。cmd 已小写，args 保留原始大小写（应用名区分大小写）。
// 返回空字符串表示不是本模块的命令。
std::string MacControlConsoleCommand(const std::string& cmd,
                                     const std::vector<std::string>& args);

// 语音识别文本本地兜底：只要小智听到了这句话就执行动作，完全不依赖云端大模型
// 是否决定调用工具，也不需要小智后台 MCP 接入点。由 application.cc 的 stt 分支
// 以弱符号方式调用（MAC_CONTROL_ENABLE=OFF 时本函数不存在，调用点自动跳过）。
// 命中返回 true；执行结果异步打印到 [LOCAL-CMD] 日志。
extern "C" bool MacVoiceCommandHook(const char* text);
