/*
 * qibao_persona.h —— 七宝妹妹的人设与刷脸门禁
 *
 * 岗哥的专属 AI 伙伴：认出主人甜甜的、像女朋友一样哄岗哥；
 * 陌生人礼貌冷淡、不执行控制。
 *
 * 门禁策略：刷脸成功解锁 10 分钟，期间任意使唤；超时重新刷脸。
 * 人脸特征存 NVS（重启不丢），录入模式抓 3 帧取平均特征。
 */
#ifndef QIBAO_PERSONA_H
#define QIBAO_PERSONA_H

#include <cstdint>
#include <string>

namespace qibao {

// 人设常量
extern const char* const kOwnerName;      // "岗哥"
extern const char* const kSelfName;       // "七宝妹妹"
extern const uint32_t kUnlockDurationMs;  // 10 分钟

// 识别结果
enum class Identity : uint8_t {
    Unknown = 0,  // 陌生人 / 没看到脸
    Owner = 1,    // 岗哥
};

// ---- 生命周期 ----
// 初始化人脸检测/识别模型 + 从 NVS 载入主人特征 + 启动后台识别任务
void Start();

// ---- 门禁状态 ----
// 当前是否处于解锁状态（10 分钟内刷过脸）
bool IsUnlocked();
// 距上次解锁的剩余毫秒数
uint32_t UnlockRemainingMs();
// 是否有已录入的主人特征
bool HasOwnerFace();

// ---- 主动刷脸 ----
// 抓一帧摄像头画面做人脸比对。返回识别结果。
// blocking=true 会等待识别完成（最多约 2 秒）；false 则异步触发。
Identity VerifyFace(bool blocking = true);
// 上一次比对的相似度分数（调试用）
float LastSimilarity();

// ---- 录入 ----
// 进入录入模式：连续抓 N 帧主人人脸取平均特征，存 NVS。
// 返回 true 表示录入成功。blocking 时等待完成。
bool EnrollOwnerFace(uint8_t samples = 3, bool blocking = true);
// 清除已录入的主人特征
void ClearOwnerFace();

// ---- 人设话术（给云端/本地 TTS 用） ----
// 根据身份返回打招呼的话。用于刷脸后开口第一句。
std::string GreetingFor(Identity identity);
// 控制指令被拒绝时的话术（陌生人想使唤她）
std::string RejectFor(Identity identity);
// 撒娇/关心话术池（随机挑一句，云端连着时可在对话中穿插）
std::string AffectionateLine();

// ---- 调试/串口 ----
// 返回状态摘要（供串口命令和 MCP 工具查询）
std::string StatusJson();

// ---- 与固件主程序的桥接 ----
// 实现 application.cc 的门禁弱符号、注册 self.qibao MCP 工具。
// 在板级构造函数里和 Start() 一起调用。
void StartBridge();

}  // namespace qibao

// C 接口：application.cc 通过弱符号调用，检查控制指令是否放行。
extern "C" bool QibaoGateAllowed(const char* command);

#endif  // QIBAO_PERSONA_H
