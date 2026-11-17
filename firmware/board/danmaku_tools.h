#ifndef DANMAKU_TOOLS_H
#define DANMAKU_TOOLS_H

#include <string>
#include <vector>

class Camera;

// 注册弹幕监听 MCP 工具（self.danmaku.read / self.danmaku.reset）。
// 重要：本函数在板级构造函数里被调用，绝不能在里面调 Board::GetInstance()
// （单例还没构造完，重入 magic static 会死锁，整个固件卡在 boot）。
// 所以必须由板子把自己的 camera_ 指针显式传进来。
void StartDanmakuTools(Camera* camera);

// 串口自测命令：macphoto [问题] / macdanmaku / macdanmakureset。
// 不认识的命令返回空串，交给下一个命令处理器。
std::string DanmakuConsoleCommand(const std::string& cmd,
                                  const std::vector<std::string>& args);

#endif  // DANMAKU_TOOLS_H
