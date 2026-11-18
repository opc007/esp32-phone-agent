#pragma once

#include <cstddef>
#include <cstdint>

// 七宝妹妹 TF 卡存储：ESP32-S31-Korvo-1 板载 microSD（SDMMC 4bit）。
// 人脸特征优先存卡（/sdcard/qibao/owner.feat），重启不丢；卡不可用时回退 NVS。
namespace qibao_sd {

// 初始化 SDMMC + 挂载 FATFS。线程安全，重复调用只初始化一次。
// 返回 true 表示挂载成功。
bool Init();

// 卡是否可用（挂载成功）。
bool Available();

// 保存主人人脸特征到卡。feat 为归一化浮点向量，len 为维度。
bool SaveFeature(const float* feat, int len);

// 从卡读取主人人脸特征。out 容量至少 kMaxFeatureLen，len 返回实际维度。
bool LoadFeature(float* out, int* len);

// 删除卡上的特征文件。
void ClearFeature();

// 挂载点路径。
constexpr const char* kMountPoint = "/sdcard";
constexpr int kMaxFeatureLen = 4096;

}  // namespace qibao_sd
