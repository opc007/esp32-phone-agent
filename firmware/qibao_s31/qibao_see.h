#pragma once

#include <cstdint>
#include <string>

namespace qibao {

// 用当前帧认物品。更新「最近看到的物品」，返回给串口/工具的一句话。
std::string SeeFrame(uint8_t* rgb, int w, int h);

// 自己抓一帧再认。没有模型时才下载，失败可以再试。
std::string SeeOnce();

// TF 卡上已经有完整模型。
bool ObjectModelOnDisk();

// 只在内存还宽裕时加载一次。不下载，失败也不重试。预览循环不要调用。
void TryPreloadObjects();

// 从串口收一段模型字节写到 TF 卡。先打印 MODEL READY，再开始收。
int ReceiveModel(int expect);

// 小窗上没有人脸时，用最近一件物品当角标。没有就是空字符串。
const char* ObjectBadge();

// 最近一次认物结果，空表示还没认到。
std::string LastObjects();

}  // namespace qibao
