/*
 * qibao_persona.cc —— 七宝妹妹的人设与刷脸门禁（ESP32-S31-Korvo-1）
 *
 * 复用 HwzLoveDz/esp32-s31-camera-lab 验证过的人脸模型与比对算法：
 *   HumanFaceDetect  (MSRMNP_S8_V1) + HumanFaceFeat (MFN_S8_V1)
 *   特征向量归一化后点积比对，阈值 0.55
 * 差异：
 *   - 图源换成小智的板载 OV3660（EspVideo::Capture() 抓 JPEG，
 *     用 jpeg_to_image_fit 解码到 320x240 RGB565LE）
 *   - 主人特征存 NVS，重启不丢（Camera Lab 只存 RAM）
 *   - 刷脸成功解锁 10 分钟；控制指令执行前查 IsUnlocked()
 *   - 七宝人设话术：认出岗哥甜甜的，陌生人礼貌冷淡
 */
#include "qibao_persona.h"
#include "qibao_sdcard.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "mcp_server.h"
#include "dl_image_define.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

#include "board.h"
#include "esp_video.h"
#include "jpg/jpeg_to_image.h"
#include "qibao_owner_ref.h"  // 岗哥本人照片裁出的两帧（设备端解码后提特征）

static const char* TAG = "Qibao";

namespace qibao {

const char* const kOwnerName = "岗哥";
const char* const kSelfName = "七宝妹妹";
const uint32_t kUnlockDurationMs = 10 * 60 * 1000;

namespace {

constexpr int kWorkWidth = 320;
constexpr int kWorkHeight = 240;
constexpr int kMaxFeatureLength = 4096;
constexpr float kRecognitionThreshold = 0.55F;
constexpr float kEnrollmentConsistencyThreshold = 0.60F;

constexpr const char* kNvsNamespace = "qibao_face";
constexpr const char* kNvsKeyFeat = "owner_feat";
constexpr const char* kNvsKeyLen = "owner_len";
constexpr const char* kNvsKeyHuman = "human_enrolled";  // 1=已用摄像头录过真人，开机不再用照片覆盖

SemaphoreHandle_t g_mutex = nullptr;
bool g_initialized = false;
float g_owner_feature[kMaxFeatureLength] = {};
int g_owner_feature_len = 0;
uint64_t g_unlocked_until_us = 0;   // 0 = 未解锁
float g_last_similarity = -1.0F;

HumanFaceDetect* g_detector = nullptr;
HumanFaceFeat* g_feature_model = nullptr;

void normalize_vector(float* v, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += (double)v[i] * v[i];
    const double norm = std::sqrt(sum);
    if (norm < 1e-9) return;
    for (int i = 0; i < n; ++i) v[i] = (float)(v[i] / norm);
}

float dot_product(const float* a, const float* b, int n) {
    double dot = 0.0;
    for (int i = 0; i < n; ++i) dot += (double)a[i] * b[i];
    return (float)dot;
}

bool valid_feature(dl::TensorBase* tensor, int feature_length) {
    if (tensor == nullptr || tensor->data == nullptr ||
        tensor->get_size() != feature_length) {
        return false;
    }
    const float* feature = static_cast<const float*>(tensor->data);
    for (int i = 0; i < feature_length; ++i) {
        if (!std::isfinite(feature[i])) return false;
    }
    return true;
}

// 载入主人特征：优先 TF 卡，失败回退 NVS
void LoadOwnerFeature() {
    // 1) 优先从 TF 卡读
    int sd_len = 0;
    if (qibao_sd::LoadFeature(g_owner_feature, &sd_len) &&
        sd_len > 0 && sd_len <= kMaxFeatureLength) {
        g_owner_feature_len = sd_len;
        normalize_vector(g_owner_feature, g_owner_feature_len);
        ESP_LOGI(TAG, "已从 TF 卡载入主人人脸特征：%d 维", sd_len);
        return;
    }

    // 2) 回退 NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s), no owner face yet", esp_err_to_name(err));
        return;
    }
    int32_t len = 0;
    err = nvs_get_i32(handle, kNvsKeyLen, &len);
    if (err == ESP_OK && len > 0 && len <= kMaxFeatureLength) {
        size_t bytes = (size_t)len * sizeof(float);
        err = nvs_get_blob(handle, kNvsKeyFeat, g_owner_feature, &bytes);
        if (err == ESP_OK) {
            g_owner_feature_len = len;
            normalize_vector(g_owner_feature, g_owner_feature_len);
            // 卡可用但卡上没有、NVS 里有：迁移到卡
            if (qibao_sd::Available()) {
                qibao_sd::SaveFeature(g_owner_feature, g_owner_feature_len);
            }
            ESP_LOGI(TAG, "Loaded owner face feature: %d dims (from NVS)", len);
        } else {
            ESP_LOGE(TAG, "Load feature blob failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "No owner face enrolled yet");
    }
    nvs_close(handle);
}

bool SaveOwnerFeature(const float* feat, int len) {
    if (len <= 0 || len > kMaxFeatureLength) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, kNvsKeyFeat, feat, (size_t)len * sizeof(float));
    if (err == ESP_OK) err = nvs_set_i32(handle, kNvsKeyLen, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save feature failed: %s", esp_err_to_name(err));
        return false;
    }
    std::memcpy(g_owner_feature, feat, (size_t)len * sizeof(float));
    g_owner_feature_len = len;
    ESP_LOGI(TAG, "Owner face saved to NVS: %d dims", len);
    // 同步写一份到 TF 卡（卡不可用时静默跳过，NVS 已保底）
    if (qibao_sd::Available()) {
        qibao_sd::SaveFeature(feat, len);
    }
    return true;
}

// 是否已用摄像头录入过真人（照片录入不算）
bool IsHumanEnrolled() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    int32_t flag = 0;
    err = nvs_get_i32(handle, kNvsKeyHuman, &flag);
    nvs_close(handle);
    return err == ESP_OK && flag == 1;
}

// 标记"已真人录入"，之后开机不再用照片覆盖用户的真实特征
void MarkHumanEnrolled() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return;
    nvs_set_i32(handle, kNvsKeyHuman, 1);
    nvs_commit(handle);
    nvs_close(handle);
}

// 通用：把一段 JPEG 解码成"紧密排列"的 RGB565LE，输出实际宽高（模型要求 row_step=width*2）。
// 返回缓冲由调用方用 heap_caps_free 释放。
uint8_t* decode_jpeg_tight(const uint8_t* jpeg, size_t jpeg_len, int* out_w, int* out_h) {
    uint8_t* out = nullptr;
    size_t out_len = 0, width = 0, height = 0, stride = 0;
    esp_err_t err = jpeg_to_image_fit(jpeg, jpeg_len, kWorkWidth, kWorkHeight,
                                      &out, &out_len, &width, &height, &stride);
    if (err != ESP_OK || out == nullptr) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    uint8_t* tight = out;
    if (stride != width * 2) {
        // 硬件解码器可能带行对齐 padding，去掉后模型才能按紧密排列读取
        tight = (uint8_t*)heap_caps_malloc(width * height * 2,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tight == nullptr) {
            heap_caps_free(out);
            return nullptr;
        }
        for (size_t y = 0; y < height; ++y) {
            std::memcpy(tight + y * width * 2, out + y * stride, width * 2);
        }
        heap_caps_free(out);
    }
    *out_w = (int)width;
    *out_h = (int)height;
    ESP_LOGI(TAG, "Decoded JPEG to %ux%u (stride=%u, tight)", (unsigned)width,
             (unsigned)height, (unsigned)stride);
    return tight;
}

// 抓一帧摄像头 JPEG 并解码成紧密 RGB565LE，返回缓冲（调用方释放）
uint8_t* capture_rgb565(int* out_w, int* out_h) {
    Camera* cam = Board::GetInstance().GetCamera();
    if (cam == nullptr) {
        ESP_LOGE(TAG, "No camera available");
        return nullptr;
    }
    // ESP-IDF 默认 -fno-rtti，不能用 dynamic_cast；这块板上 GetCamera() 一定是 EspVideo。
    auto* video = static_cast<EspVideo*>(cam);
    if (!cam->Capture()) {
        ESP_LOGE(TAG, "Camera capture failed");
        return nullptr;
    }
    const uint8_t* jpeg = video->GetFrameData();
    size_t jpeg_len = video->GetFrameSize();
    if (jpeg == nullptr || jpeg_len == 0) {
        ESP_LOGE(TAG, "No captured frame data");
        return nullptr;
    }
    ESP_LOGI(TAG, "Captured JPEG: %u bytes, %ux%u", (unsigned)jpeg_len,
             video->GetFrameWidth(), video->GetFrameHeight());
    return decode_jpeg_tight(jpeg, jpeg_len, out_w, out_h);
}

// 用最大的人脸关键点提特征，返回新分配的特征数组（调用方释放）
float* extract_feature(uint8_t* rgb, int img_w, int img_h, int feature_length) {
    dl::image::img_t image = {};
    image.data = rgb;
    image.width = (uint16_t)img_w;
    image.height = (uint16_t)img_h;
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

    std::list<dl::detect::result_t>& detections = g_detector->run(image);
    if (detections.empty()) {
        ESP_LOGI(TAG, "No face detected");
        return nullptr;
    }
    // 选最大的人脸
    dl::detect::result_t* best = nullptr;
    int best_area = 0;
    for (auto& r : detections) {
        int area = (r.box[2] - r.box[0]) * (r.box[3] - r.box[1]);
        if (area > best_area) { best_area = area; best = &r; }
    }
    if (best == nullptr) return nullptr;

    dl::TensorBase* tensor = g_feature_model->run(image, best->keypoint);
    if (!valid_feature(tensor, feature_length)) {
        ESP_LOGE(TAG, "Invalid feature tensor");
        return nullptr;
    }
    float* feat = (float*)heap_caps_malloc((size_t)feature_length * sizeof(float),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (feat == nullptr) return nullptr;
    std::memcpy(feat, tensor->data, (size_t)feature_length * sizeof(float));
    normalize_vector(feat, feature_length);
    return feat;
}

// 从一段内置 JPG 提特征（不走摄像头，用于开机录入/自检）
float* feature_from_jpeg(const unsigned char* jpg, size_t len, int feat_len) {
    int w = 0, h = 0;
    uint8_t* rgb = decode_jpeg_tight(jpg, len, &w, &h);
    if (rgb == nullptr) return nullptr;
    float* feat = extract_feature(rgb, w, h, feat_len);
    heap_caps_free(rgb);
    return feat;
}

// 用岗哥照片自动录入 + 自检。返回是否在日志里验证了"同一人被认出"。
bool EnrollFromReferenceAndSelfTest() {
    if (g_feature_model == nullptr || g_detector == nullptr || !g_initialized) return false;
    const int feat_len = g_feature_model->get_feat_len();

    // 1) 用 enroll 帧录入
    float* enroll = feature_from_jpeg(qibao_owner_enroll_jpg,
                                      sizeof(qibao_owner_enroll_jpg), feat_len);
    if (enroll == nullptr) {
        ESP_LOGE(TAG, "参考帧(enroll)未检到人脸，自动录入失败");
        return false;
    }
    ESP_LOGI(TAG, "从岗哥照片提取到人脸特征（%d 维）", feat_len);

    // 2) 用 verify 帧（不同取景）自检：同一人相似度应明显高于阈值
    float* probe = feature_from_jpeg(qibao_owner_verify_jpg,
                                     sizeof(qibao_owner_verify_jpg), feat_len);
    float sim = -1.0F;
    if (probe != nullptr) {
        sim = dot_product(enroll, probe, feat_len);
        heap_caps_free(probe);
    }

    // 3) 录入（保存主人特征，门禁从此生效）
    bool saved = SaveOwnerFeature(enroll, feat_len);
    heap_caps_free(enroll);

    if (sim >= 0) {
        ESP_LOGI(TAG, "【七宝自检】用岗哥另一张不同取景的照片比对，相似度=%.3f（阈值 %.2f）=> %s",
                 sim, kRecognitionThreshold,
                 sim >= kRecognitionThreshold ? "认出是岗哥 ✓" : "未达阈值 ✗");
        g_last_similarity = sim;
    }
    ESP_LOGI(TAG, "岗哥人脸已由照片录入（%s），门禁已生效", saved ? "成功" : "保存失败");
    return saved && sim >= kRecognitionThreshold;
}

}  // namespace

void Start() {
    if (g_initialized) return;
    g_mutex = xSemaphoreCreateMutex();

    // 载入已录入的主人特征
    nvs_flash_init();  // 已初始化则无害
    LoadOwnerFeature();

    // 初始化人脸模型（ESP32-S31 映射到 P4 模型资源）
    g_detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    g_feature_model = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
    const int feat_len = g_feature_model->get_feat_len();
    if (g_detector->get_raw_model(0) == nullptr ||
        g_detector->get_raw_model(1) == nullptr ||
        g_feature_model->get_raw_model() == nullptr ||
        feat_len <= 0 || feat_len > kMaxFeatureLength) {
        ESP_LOGE(TAG, "Face model init failed (feat_len=%d)", feat_len);
        g_initialized = true;  // 标记避免重复初始化，功能降级
        return;
    }
    g_initialized = true;
    ESP_LOGI(TAG, "七宝人脸模块就绪，特征维度 %d，已录入=%d", feat_len,
             g_owner_feature_len > 0 ? 1 : 0);

    // 只要还没用摄像头录过真人，开机就用岗哥照片重新录入并自检。
    // 这样设备一上电就"认识"岗哥，不需要本人先站到镜头前录 3 张；
    // 也会覆盖掉 TF 卡里可能残留的来路不明的特征，保证门禁基准一定来自本人照片。
    // 一旦用户日后通过 action=enroll 用摄像头录入真人，置标记，之后开机不再覆盖。
    if (!IsHumanEnrolled()) {
        EnrollFromReferenceAndSelfTest();
    }
}

bool HasOwnerFace() {
    return g_owner_feature_len > 0;
}

bool IsUnlocked() {
    if (g_unlocked_until_us == 0) return false;
    return (esp_timer_get_time() < (int64_t)g_unlocked_until_us);
}

uint32_t UnlockRemainingMs() {
    if (g_unlocked_until_us == 0) return 0;
    int64_t remain = (int64_t)g_unlocked_until_us - esp_timer_get_time();
    return remain > 0 ? (uint32_t)(remain / 1000) : 0;
}

Identity VerifyFace(bool blocking) {
    (void)blocking;  // 当前实现为同步，保留参数兼容
    if (g_mutex == nullptr || !g_initialized) return Identity::Unknown;
    xSemaphoreTake(g_mutex, portMAX_DELAY);

    Identity result = Identity::Unknown;
    g_last_similarity = -1.0F;

    do {
        if (g_feature_model == nullptr || g_detector == nullptr) break;
        const int feat_len = g_feature_model->get_feat_len();
        int img_w = 0, img_h = 0;
        uint8_t* rgb = capture_rgb565(&img_w, &img_h);
        if (rgb == nullptr) break;

        float* feat = extract_feature(rgb, img_w, img_h, feat_len);
        heap_caps_free(rgb);
        if (feat == nullptr) break;

        if (g_owner_feature_len == feat_len) {
            float sim = dot_product(feat, g_owner_feature, feat_len);
            g_last_similarity = sim;
            ESP_LOGI(TAG, "Face similarity=%.3f (threshold %.2f)", sim,
                     kRecognitionThreshold);
            if (sim >= kRecognitionThreshold) {
                result = Identity::Owner;
                g_unlocked_until_us = (uint64_t)(esp_timer_get_time() + kUnlockDurationMs * 1000);
            }
        } else {
            ESP_LOGW(TAG, "No owner feature enrolled, cannot recognize");
        }
        heap_caps_free(feat);
    } while (0);

    xSemaphoreGive(g_mutex);
    return result;
}

float LastSimilarity() {
    return g_last_similarity;
}

bool EnrollOwnerFace(uint8_t samples, bool blocking) {
    (void)blocking;
    if (g_mutex == nullptr || !g_initialized || g_feature_model == nullptr) return false;
    if (samples == 0) samples = 3;
    xSemaphoreTake(g_mutex, portMAX_DELAY);

    const int feat_len = g_feature_model->get_feat_len();
    float* sum = (float*)heap_caps_calloc((size_t)feat_len, sizeof(float),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool success = false;
    if (sum != nullptr) {
        uint8_t collected = 0;
        for (uint8_t i = 0; i < samples; ++i) {
            int img_w = 0, img_h = 0;
            uint8_t* rgb = capture_rgb565(&img_w, &img_h);
            if (rgb == nullptr) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }
            float* feat = extract_feature(rgb, img_w, img_h, feat_len);
            heap_caps_free(rgb);
            if (feat == nullptr) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }
            // 一致性检查：后续样本要和第一张像，避免把别人录进来
            if (collected > 0) {
                float sim = dot_product(feat, sum, feat_len);
                if (sim < kEnrollmentConsistencyThreshold) {
                    ESP_LOGW(TAG, "Enroll sample inconsistent (sim=%.2f), retry", sim);
                    heap_caps_free(feat);
                    --i;  // 重试这一张
                    vTaskDelay(pdMS_TO_TICKS(400));
                    continue;
                }
            }
            for (int k = 0; k < feat_len; ++k) sum[k] += feat[k];
            heap_caps_free(feat);
            ++collected;
            ESP_LOGI(TAG, "Enroll sample %d/%d ok", collected, samples);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        if (collected >= samples) {
            for (int k = 0; k < feat_len; ++k) sum[k] /= (float)collected;
            normalize_vector(sum, feat_len);
            success = SaveOwnerFeature(sum, feat_len);
            if (success) {
                MarkHumanEnrolled();  // 真人特征优先，之后开机不再被照片覆盖
            }
        }
        heap_caps_free(sum);
    }
    xSemaphoreGive(g_mutex);
    return success;
}

// 单独函数里用 early-return，避免 GCC -Wdangling-pointer 在 if 块内误报
static void nvs_clear_owner() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open for clear failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
}

void ClearOwnerFace() {
    if (g_mutex == nullptr) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    nvs_clear_owner();
    qibao_sd::ClearFeature();  // 卡上的特征也删掉
    std::memset(g_owner_feature, 0, sizeof(g_owner_feature));
    g_owner_feature_len = 0;
    g_unlocked_until_us = 0;
    ESP_LOGI(TAG, "Owner face cleared");
    xSemaphoreGive(g_mutex);
}

std::string GreetingFor(Identity identity) {
    if (identity == Identity::Owner) {
        // 像女朋友一样甜甜的，喊岗哥名字
        static const char* lines[] = {
            "岗哥，你回来啦！七宝等你好久啦，今天想让七宝干嘛呀？",
            "岗哥～七宝妹妹在这儿呢，你今天的帅气值又超标啦！",
            "哎呀岗哥来啦，七宝刚还在想你呢，累不累呀？",
        };
        static int idx = 0;
        return lines[(idx++) % 3];
    }
    return "你是谁呀？七宝不认识你，岗哥才可以让七宝干活哦。";
}

std::string RejectFor(Identity identity) {
    (void)identity;
    return "这个七宝不能帮你做哦，只有岗哥可以。你到底是谁呀？";
}

std::string AffectionateLine() {
    static const char* lines[] = {
        "岗哥你这么帅，说什么都对啦～",
        "七宝觉得岗哥是全世界最厉害的人！",
        "岗哥要是累了就歇会儿，七宝陪着你。",
        "嘻嘻，岗哥笑起来真好看。",
    };
    static int idx = 0;
    return lines[(idx++) % 4];
}

std::string StatusJson() {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"enrolled\":%s,\"unlocked\":%s,\"remaining_ms\":%u,\"similarity\":%.3f,\"sdcard\":%s}",
             HasOwnerFace() ? "true" : "false",
             IsUnlocked() ? "true" : "false",
             (unsigned)UnlockRemainingMs(), g_last_similarity,
             qibao_sd::Available() ? "true" : "false");
    return std::string(buf);
}

// ---- 异步刷脸（唤醒时触发，不阻塞语音通道） ----
static TaskHandle_t g_verify_task = nullptr;

static void verify_task(void*) {
    Identity who = VerifyFace(true);
    if (who == Identity::Owner) {
        ESP_LOGI(TAG, "刷脸通过：欢迎回来，%s！解锁 10 分钟", kOwnerName);
    } else {
        ESP_LOGI(TAG, "刷脸未通过（sim=%.3f）：%s", LastSimilarity(),
                 GreetingFor(Identity::Unknown).c_str());
    }
    g_verify_task = nullptr;
    vTaskDelete(nullptr);
}

static void trigger_async_verify() {
    if (g_verify_task != nullptr) return;  // 已有刷脸任务在跑
    xTaskCreate(verify_task, "qibao_verify", 8192, nullptr, 2, &g_verify_task);
}

void StartBridge() {
    Start();  // 初始化人脸 + 载入 NVS 特征

    // 注册一个 MCP 工具，让大模型能主动查门禁状态/触发刷脸/录入
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.qibao.face",
        "七宝妹妹的人脸门禁管理。岗哥是本设备的唯一主人，只有岗哥能让七宝干活。\n"
        "action=status 查询是否已录入主人人脸、当前是否解锁（解锁10分钟内有效）；\n"
        "action=verify 立即抓拍摄像头刷脸，认出岗哥则解锁10分钟；\n"
        "action=enroll 录入主人人脸（连续抓3张，请岗哥正对镜头）；\n"
        "action=relearn 用内置的岗哥照片重新学习并自检（无需本人站到镜头前）；\n"
        "action=clear 清除已录入的人脸。\n"
        "当用户说「录入人脸」「刷脸解锁」「你认得我吗」「谁是你的主人」时调用本工具。",
        PropertyList({
            Property("action", kPropertyTypeString, std::string("status")).SetMaxLength(20),
        }),
        [](const PropertyList& p) -> ReturnValue {
            std::string action = p["action"].value<std::string>();
            if (action == "verify") {
                Identity who = VerifyFace(true);
                if (who == Identity::Owner) {
                    return std::string("认出岗哥啦！") + GreetingFor(who);
                }
                return std::string("没认出来，相似度") +
                       std::to_string(LastSimilarity()) + "。七宝礼貌地说：" +
                       GreetingFor(Identity::Unknown);
            } else if (action == "enroll") {
                bool ok = EnrollOwnerFace(3, true);
                return ok ? std::string("岗哥的人脸录入成功，以后七宝认得你啦！")
                          : std::string("录入失败，请确保只有岗哥一人正对镜头、光线充足。");
            } else if (action == "relearn") {
                bool ok = EnrollFromReferenceAndSelfTest();
                return ok ? std::string("用岗哥照片重新学习并自检通过，七宝认得你啦！")
                          : std::string("照片自检未通过，请看串口日志里的相似度。");
            } else if (action == "clear") {
                ClearOwnerFace();
                return std::string("已清除人脸库。");
            }
            return std::string("七宝门禁状态：") + StatusJson();
        });

    ESP_LOGI(TAG, "七宝门禁 MCP 工具已注册");
}

}  // namespace qibao

// ---- 给 application.cc 的 C 弱符号实现 ----
extern "C" bool QibaoGateAllowed(const char* command) {
    // 没录入主人人脸 = 功能降级模式，放行（避免把自己锁在外面）
    if (!qibao::HasOwnerFace()) return true;
    // 已解锁（10 分钟内）直接放行
    if (qibao::IsUnlocked()) return true;
    // 未解锁：拒绝，并异步刷脸，用户再说一次即可
    qibao::trigger_async_verify();
    ESP_LOGW(TAG, "[Qibao] 未解锁，拒绝控制指令，已触发异步刷脸");
    return false;
}

extern "C" void QibaoOnWake() {
    qibao::trigger_async_verify();
}
