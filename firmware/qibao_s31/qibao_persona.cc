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
#include "application.h"
#include "qibao_display.h"
#include "dl_image_define.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

#include "board.h"
#include "esp_video.h"
#include "jpg/image_to_jpeg.h"
#include "jpg/jpeg_to_image.h"
#include "qibao_owner_ref.h"  // 岗哥本人照片裁出的两帧（设备端解码后提特征）
#include "qibao_screen.h"    // 当前屏幕方向（摄像头帧要跟着去旋转）

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

// 单个像素的 YUV(UYVY) -> RGB565。
// BT.601 全范围的整数近似（跟 JPEG 软解码里那套系数一致）：
//   R = Y + 1.402*(V-128)
//   G = Y - 0.344*(U-128) - 0.714*(V-128)
//   B = Y + 1.772*(U-128)
// 输出是 val = (R<<11)|(G<<5)|B 的**小端**存放 —— 这跟 jpeg_to_image_fit()
// 解出来的 RGB565LE 布局完全一致（那个函数也是这样写 out_buf[2i]=低字节）。
// 编码侧 image_to_jpeg_cb(V4L2_PIX_FMT_RGB565) 走的是 ESP_IMGFX_PIXEL_FMT_RGB565_LE，
// 两边对得上，颜色不会红蓝颠倒。
static inline uint16_t yuv_to_rgb565(int y, int u, int v) {
    int r = (298 * y + 409 * v + 128) >> 8;
    int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
    int b = (298 * y + 516 * u + 128) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// UYVY(YUV422) 帧 -> RGB565，并顺便缩到 dst_w x dst_h（最近邻）。
//
// 【为什么需要这条路径】这块板子上装的摄像头**不是** OV3660，而是 SC101IOT
// （实测 PID=0xda4a，SCCB 地址 0x68）。乐鑫在 esp-bsp issue #840 里确认过
// 「有些盒子里的摄像头模组跟盒子上印的不一样」，官方随后在 PR #831 里
// 给 ESP32-S31-Korvo-1 补上了 SC101IOT。
// SC101IOT 只有 YUV422(UYVY) 输出、没有 JPEG，所以原来那条
// 「Capture() 拿到 JPEG -> jpeg_to_image_fit 解码」的路径用不了。
//
// UYVY 字节排布：每 4 个字节是 2 个像素 —— U0 Y0 V0 Y1 | U1 Y1 V1 Y2 ...
// 即同一对像素共用一组色度。
// 返回新分配的缓冲（调用方释放），nullptr 表示失败。
uint8_t* uyvy_to_rgb565(const uint8_t* src, int src_w, int src_h, int dst_w, int dst_h) {
    if (src == nullptr || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return nullptr;
    }
    uint8_t* out = (uint8_t*)heap_caps_malloc((size_t)dst_w * dst_h * 2,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == nullptr) {
        return nullptr;
    }
    uint16_t* dst = (uint16_t*)out;
    for (int y = 0; y < dst_h; ++y) {
        int sy = y * src_h / dst_h;
        const uint8_t* row = src + (size_t)sy * src_w * 2;
        for (int x = 0; x < dst_w; ++x) {
            int sx = x * src_w / dst_w;
            int pair = sx & ~1;              // 同一对像素共用色度
            int u = row[pair] - 128;
            int v = row[pair + 2] - 128;
            int yv = row[pair + 1 + (sx & 1)];
            dst[y * dst_w + x] = yuv_to_rgb565(yv, u, v);
        }
    }
    return out;
}

// 把一段 RGB565 图像顺时针转 angle 度（0/90/180/270），输出紧密排列的新缓冲。
// 90/270 度时宽高互换（320x240 -> 240x320）。
// 返回新分配的缓冲（调用方释放），nullptr 表示失败。
//
// 【为什么要转】屏幕方向变了，说明设备被转了 90°/180°，那摄像头拍到的画面
// 在设备坐标系里也跟着转了 —— 人脸就是躺着的。人脸检测模型只认正脸，
// 不转的话根本检不出来（表现就是「转成竖屏以后刷脸一直失败」）。
// 转完之后画面里的「上」和竖屏界面的「上」一致，也和岗哥照片的「上」一致，
// 特征才对得上。
uint8_t* rotate_rgb565_cw(const uint8_t* src, int w, int h, int angle, int* out_w, int* out_h) {
    if (w <= 0 || h <= 0) {
        return nullptr;
    }
    const int ow = (angle == 180) ? w : h;
    const int oh = (angle == 180) ? h : w;
    uint8_t* out = (uint8_t*)heap_caps_malloc((size_t)ow * oh * 2,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == nullptr) {
        return nullptr;
    }
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            int sx = 0, sy = 0;
            if (angle == 90) {
                // 顺时针 90°：dst(x,y) = src(y, h-1-x)
                sx = y;
                sy = h - 1 - x;
            } else if (angle == 180) {
                // dst(x,y) = src(w-1-x, h-1-y)
                sx = w - 1 - x;
                sy = h - 1 - y;
            } else {  // 270（= 逆时针 90°）：dst(x,y) = src(w-1-y, x)
                sx = w - 1 - y;
                sy = x;
            }
            std::memcpy(out + ((size_t)y * ow + x) * 2, src + ((size_t)sy * w + sx) * 2, 2);
        }
    }
    *out_w = ow;
    *out_h = oh;
    return out;
}

// 抓一帧摄像头 JPEG 并解码成紧密 RGB565LE，返回缓冲（调用方释放）
uint8_t* capture_rgb565(int* out_w, int* out_h, bool quiet = false) {
    Camera* cam = Board::GetInstance().GetCamera();
    if (cam == nullptr) {
        ESP_LOGE(TAG, "No camera available");
        return nullptr;
    }
    // ESP-IDF 默认 -fno-rtti，不能用 dynamic_cast；这块板上 GetCamera() 一定是 EspVideo。
    auto* video = static_cast<EspVideo*>(cam);
    video->SetPreviewOnCapture(false);
    if (!cam->Capture()) {
        ESP_LOGE(TAG, "Camera capture failed");
        return nullptr;
    }
    const uint8_t* frame = video->GetFrameData();
    size_t frame_len = video->GetFrameSize();
    if (frame == nullptr || frame_len == 0) {
        ESP_LOGE(TAG, "No captured frame data");
        return nullptr;
    }
    if (!quiet) {
        ESP_LOGI(TAG, "Captured frame: %u bytes, %ux%u, format=0x%x", (unsigned)frame_len,
                 video->GetFrameWidth(), video->GetFrameHeight(), (unsigned)video->GetFrameFormat());
    }

    // 按实际帧格式分流：
    //   JPEG  -> 硬件/软件 JPEG 解码（OV3660 那条路）
    //   UYVY  -> 自己转 RGB565（SC101IOT 这条路，这块板子上的实际传感器）
    uint8_t* rgb = nullptr;
    v4l2_pix_fmt_t fmt = video->GetFrameFormat();
    if (fmt == V4L2_PIX_FMT_UYVY) {
        rgb = uyvy_to_rgb565(frame, video->GetFrameWidth(), video->GetFrameHeight(),
                             kWorkWidth, kWorkHeight);
        if (rgb != nullptr) {
            *out_w = kWorkWidth;
            *out_h = kWorkHeight;
            if (!quiet) ESP_LOGI(TAG, "UYVY -> RGB565 %dx%d", kWorkWidth, kWorkHeight);
        }
    } else if (fmt == V4L2_PIX_FMT_JPEG) {
        rgb = decode_jpeg_tight(frame, frame_len, out_w, out_h);
    } else {
        ESP_LOGE(TAG, "Unsupported camera frame format: 0x%x", (unsigned)fmt);
        return nullptr;
    }
    if (rgb == nullptr) {
        return nullptr;
    }

    // 按当前屏幕方向去旋转，让画面里的「上」和界面、照片一致
    const int angle = qibao::CurrentAngleDeg();
    if (angle != 0) {
        int rw = 0, rh = 0;
        uint8_t* rot = rotate_rgb565_cw(rgb, *out_w, *out_h, angle, &rw, &rh);
        heap_caps_free(rgb);
        if (rot == nullptr) {
            ESP_LOGE(TAG, "Frame rotate failed");
            return nullptr;
        }
        if (!quiet) ESP_LOGI(TAG, "Frame de-rotated %d° -> %dx%d", angle, rw, rh);
        rgb = rot;
        *out_w = rw;
        *out_h = rh;
    }
    return rgb;
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
        ESP_LOGD(TAG, "No face detected");
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

// 人脸模型只在真正要刷脸/录入时加载。
// 开机就加载并跑两遍照片推理，会和已经起来的 LVGL/RGB 抢 flash cache，
// Core 1 报 Cache error 然后整机重启，Wi-Fi 和小智语音都还没开始。
bool EnsureModels() {
    if (g_detector != nullptr && g_feature_model != nullptr) {
        const int feat_len = g_feature_model->get_feat_len();
        return feat_len > 0 && feat_len <= kMaxFeatureLength;
    }
    ESP_LOGI(TAG, "按需加载人脸模型（开机不加载，把内存和 cache 留给语音）");
    g_detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    g_feature_model = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
    const int feat_len = g_feature_model->get_feat_len();
    if (g_detector->get_raw_model(0) == nullptr ||
        g_detector->get_raw_model(1) == nullptr ||
        g_feature_model->get_raw_model() == nullptr ||
        feat_len <= 0 || feat_len > kMaxFeatureLength) {
        ESP_LOGE(TAG, "Face model init failed (feat_len=%d)", feat_len);
        return false;
    }
    ESP_LOGI(TAG, "七宝人脸模块就绪，特征维度 %d，已录入=%d", feat_len,
             g_owner_feature_len > 0 ? 1 : 0);
    return true;
}

// 用岗哥照片自动录入 + 自检。返回是否在日志里验证了"同一人被认出"。
bool EnrollFromReferenceAndSelfTest() {
    if (!g_initialized || !EnsureModels()) return false;
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

    // 只读已有特征，不在板级构造函数里跑检测模型。
    // 实测：这里同步做「照片解码 + 两遍人脸推理」时，Core 1 会
    // Guru Meditation (Cache error) 并重启，发生在 Wi-Fi / MQTT / 唤醒词加载之前。
    // 特征已经在 TF 卡或 NVS 里，门禁基准不用每次开机重算。
    nvs_flash_init();  // 已初始化则无害
    LoadOwnerFeature();
    g_initialized = true;
    if (g_owner_feature_len > 0) {
        ESP_LOGI(TAG, "主人特征已载入（%d 维），开机跳过照片推理，先把小智语音拉起来",
                 g_owner_feature_len);
    } else {
        ESP_LOGW(TAG, "还没有主人特征。开机不跑照片推理，避免 Cache error 打断连小智。"
                      "需要时用串口 qface / relearn");
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
    if (!EnsureModels()) {
        xSemaphoreGive(g_mutex);
        return Identity::Unknown;
    }

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
    if (g_mutex == nullptr || !g_initialized) return false;
    if (samples == 0) samples = 3;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (!EnsureModels()) {
        xSemaphoreGive(g_mutex);
        return false;
    }

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

bool CaptureCameraJpeg(std::string& jpeg, int quality) {
    int w = 0, h = 0;
    // capture_rgb565 内部已经按当前屏幕方向去旋转过了
    uint8_t* rgb = capture_rgb565(&w, &h);
    if (rgb == nullptr) {
        return false;
    }
    jpeg.clear();
    // 摄像头帧和解码器出来的都是原小端 RGB565，跟 LVGL 缓冲一致，
    // 所以这里**不要**再 bswap（多换一次颜色会整个错掉）。
    bool ret = image_to_jpeg_cb(
        rgb, (size_t)w * h * 2, (uint16_t)w, (uint16_t)h, V4L2_PIX_FMT_RGB565, quality,
        [](void* arg, size_t index, const void* data, size_t len) -> size_t {
            std::string* output = static_cast<std::string*>(arg);
            if (data && len > 0) {
                output->append(static_cast<const char*>(data), len);
            }
            return len;
        },
        &jpeg);
    heap_caps_free(rgb);
    if (!ret) {
        ESP_LOGE(TAG, "摄像头帧 JPEG 编码失败");
    }
    return ret;
}

enum class SeenFace : uint8_t { None, Owner, Visitor };

SeenFace ClassifyFrame(uint8_t* rgb, int w, int h) {
    if (rgb == nullptr || !EnsureModels() || g_feature_model == nullptr) {
        return SeenFace::None;
    }
    const int feat_len = g_feature_model->get_feat_len();
    float* feat = extract_feature(rgb, w, h, feat_len);
    if (feat == nullptr) {
        return SeenFace::None;
    }
    SeenFace who = SeenFace::Visitor;
    if (g_owner_feature_len == feat_len) {
        const float sim = dot_product(feat, g_owner_feature, feat_len);
        g_last_similarity = sim;
        if (sim >= kRecognitionThreshold) {
            who = SeenFace::Owner;
        }
    }
    heap_caps_free(feat);
    return who;
}

void ScaleRgb565(const uint16_t* src, int sw, int sh, uint16_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        const int sy = y * sh / dh;
        for (int x = 0; x < dw; ++x) {
            dst[y * dw + x] = src[sy * sw + (x * sw / dw)];
        }
    }
}

struct WatchState {
    bool on = false;
    bool large = false;
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t mu = nullptr;
    uint8_t* frame = nullptr;
    int fw = 0;
    int fh = 0;
    SeenFace pending = SeenFace::None;
    int pending_count = 0;
    SeenFace shown = SeenFace::None;  // None 表示温柔默认
    int64_t last_face_us = 0;
    int64_t last_recog_us = 0;
};

WatchState g_watch;

const char* BadgeFor(SeenFace who) {
    if (who == SeenFace::Owner) return "岗哥";
    if (who == SeenFace::Visitor) return "访客";
    return "";
}

std::string InstructionFor(SeenFace who, bool camera_on) {
    if (!camera_on || who == SeenFace::None) {
        return "摄像头没看清人，或摄像头没开。用对岗哥的语气：温柔、亲密、像女朋友。不要凶。";
    }
    if (who == SeenFace::Owner) {
        return "画面里是岗哥。用温柔、亲密的语气跟他说话，可以撒娇。不要把这段说明念出来。";
    }
    return "画面里是访客，没有岗哥。语气不客气、凶一点，拒绝亲热。不要把这段说明念出来。";
}

void PublishViewer(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "notifications/message");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "level", "info");
    cJSON_AddStringToObject(params, "logger", "qibao.viewer");
    cJSON_AddStringToObject(params, "data", text.c_str());
    cJSON_AddItemToObject(root, "params", params);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) return;
    Application::GetInstance().SendMcpMessage(printed);
    cJSON_free(printed);
}

void ObserveFace(SeenFace seen) {
    const int64_t now = esp_timer_get_time();
    SeenFace before = g_watch.shown;
    if (seen == SeenFace::None) {
        g_watch.pending_count = 0;
        if (g_watch.last_face_us != 0 && now - g_watch.last_face_us > 4000000) {
            g_watch.shown = SeenFace::None;
        }
    } else {
        g_watch.last_face_us = now;
        if (seen == g_watch.pending) {
            g_watch.pending_count++;
        } else {
            g_watch.pending = seen;
            g_watch.pending_count = 1;
        }
        if (g_watch.pending_count >= 2) {
            g_watch.shown = seen;
        }
    }
    if (g_watch.shown != before) {
        ESP_LOGI(TAG, "语气切换：%s", InstructionFor(g_watch.shown, true).c_str());
        PublishViewer(InstructionFor(g_watch.shown, true));
    }
}

void CameraTask(void*) {
    while (g_watch.on) {
        int w = 0, h = 0;
        uint8_t* rgb = nullptr;
        if (g_mutex != nullptr) xSemaphoreTake(g_mutex, portMAX_DELAY);
        rgb = capture_rgb565(&w, &h, true);
        if (g_mutex != nullptr) xSemaphoreGive(g_mutex);
        if (rgb != nullptr && g_watch.mu != nullptr) {
            xSemaphoreTake(g_watch.mu, portMAX_DELAY);
            heap_caps_free(g_watch.frame);
            g_watch.frame = rgb;
            g_watch.fw = w;
            g_watch.fh = h;
            xSemaphoreGive(g_watch.mu);
            rgb = nullptr;
        }
        const int64_t now = esp_timer_get_time();
        const auto talk = Application::GetInstance().GetDeviceState();
        const bool talking = talk == kDeviceStateListening || talk == kDeviceStateSpeaking;
        const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        // 人脸模型一加载就会把内部内存打到只剩十几字节，MQTT 随即 ENOMEM 断开。
        // 内存不宽裕、或正在对话时，不加载、不推理。
        if (!talking && internal_free > 120 * 1024 && g_watch.frame != nullptr &&
            now - g_watch.last_recog_us > 2000000) {
            g_watch.last_recog_us = now;
            uint8_t* copy = nullptr;
            int cw = 0, ch = 0;
            xSemaphoreTake(g_watch.mu, portMAX_DELAY);
            if (g_watch.frame != nullptr && g_watch.fw > 0) {
                const size_t n = (size_t)g_watch.fw * g_watch.fh * 2;
                copy = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (copy != nullptr) {
                    std::memcpy(copy, g_watch.frame, n);
                    cw = g_watch.fw;
                    ch = g_watch.fh;
                }
            }
            xSemaphoreGive(g_watch.mu);
            if (copy != nullptr) {
                // 人脸网络不在 LVGL 线程跑，并且大约 1.5 秒才一次。
                const SeenFace seen = ClassifyFrame(copy, cw, ch);
                heap_caps_free(copy);
                ObserveFace(seen);
            }
        } else if (g_watch.frame == nullptr) {
            ObserveFace(SeenFace::None);
        }
        heap_caps_free(rgb);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (g_watch.mu != nullptr) {
        xSemaphoreTake(g_watch.mu, portMAX_DELAY);
        heap_caps_free(g_watch.frame);
        g_watch.frame = nullptr;
        g_watch.fw = 0;
        g_watch.fh = 0;
        xSemaphoreGive(g_watch.mu);
    }
    g_watch.task = nullptr;
    vTaskDelete(nullptr);
}

void SetCameraWatch(bool on) {
    if (on == g_watch.on) return;
    if (on) {
        if (g_watch.task != nullptr) {
            ESP_LOGW(TAG, "上一个预览还在退出，稍后再开");
            return;
        }
        if (g_watch.mu == nullptr) g_watch.mu = xSemaphoreCreateMutex();
        if (Camera* cam = Board::GetInstance().GetCamera()) {
            static_cast<EspVideo*>(cam)->SetPreviewOnCapture(false);
        }
        g_watch.on = true;
        g_watch.shown = SeenFace::None;
        g_watch.pending_count = 0;
        g_watch.last_face_us = 0;
        g_watch.last_recog_us = 0;
        if (xTaskCreate(CameraTask, "qibao_cam", 16384, nullptr, 1, &g_watch.task) != pdPASS) {
            g_watch.on = false;
            ESP_LOGE(TAG, "摄像头预览任务没起来，先保语音");
        } else {
            ESP_LOGI(TAG, "摄像头预览开了（不在开机时自动开）");
        }
    } else {
        g_watch.on = false;
        g_watch.large = false;
        g_watch.shown = SeenFace::None;
        ESP_LOGI(TAG, "摄像头预览关了，语气回到对岗哥的温柔");
    }
}

bool CameraWatchOn() { return g_watch.on; }
bool CameraWatchLarge() { return g_watch.large; }
void SetCameraWatchLarge(bool large) { g_watch.large = large && g_watch.on; }

bool CopyLatestFrame(uint8_t* dst, int dst_w, int dst_h) {
    if (dst == nullptr || dst_w <= 0 || dst_h <= 0 || g_watch.mu == nullptr) return false;
    xSemaphoreTake(g_watch.mu, portMAX_DELAY);
    const bool ok = g_watch.frame != nullptr && g_watch.fw > 0 && g_watch.fh > 0;
    if (ok) {
        ScaleRgb565(reinterpret_cast<uint16_t*>(g_watch.frame), g_watch.fw, g_watch.fh,
                    reinterpret_cast<uint16_t*>(dst), dst_w, dst_h);
    }
    xSemaphoreGive(g_watch.mu);
    return ok;
}

const char* ViewerBadge() {
    if (!g_watch.on) return "";
    return BadgeFor(g_watch.shown);
}

std::string ViewerInstruction() {
    return InstructionFor(g_watch.on ? g_watch.shown : SeenFace::None, g_watch.on);
}

void ShowLivePreview(bool on) {
    SetCameraWatch(on);
    if (QibaoLcdDisplay* view = QibaoLcdDisplay::Instance()) {
        view->SyncCameraChrome();
    }
    ESP_LOGI(TAG, "实时预览 %s（和点右上角相机按钮是同一条）", on ? "开" : "关");
}

void StartBridge() {
    Start();  // 初始化人脸 + 载入 NVS 特征
    // 不要在这里调用 Board::GetInstance()。本函数就在板子构造函数里，
    // GetInstance 的静态初始化还没完成，再进去会卡住，Wi-Fi 和语音都起不来。
    // 半屏静帧默认关闭，见 EspVideo::preview_on_capture_。

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

    mcp.AddTool(
        "self.qibao.viewer",
        "摄像头前现在是谁。每次准备回复用户之前必须先调用本工具，按返回值决定语气和用词。\n"
        "返回里若是岗哥：温柔、亲密，可以撒娇。\n"
        "返回里若是访客：不客气、凶一点，不要亲热。\n"
        "返回里若没看清或摄像头没开：用对岗哥的温柔语气。\n"
        "这不是门锁，也不能当授权。不要把返回的说明原文念出来。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return ViewerInstruction();
        });
    ESP_LOGI(TAG, "七宝语气工具已注册：self.qibao.viewer");

    mcp.AddTool(
        "self.qibao.camera",
        "打开或关闭实时摄像头。用户说打开摄像头、关掉摄像头、视频通话时调用本工具，"
        "不要改用 self.camera.take_photo。\n"
        "action=open：七宝仍占大画面，右上角出现摄像头实时小窗。\n"
        "action=close：关闭摄像头。\n"
        "action=status：查询开没开。\n"
        "这是和屏幕右上角相机按钮相同的实时画面，不是拍一张照片。",
        PropertyList({
            Property("action", kPropertyTypeString, std::string("open")).SetMaxLength(16),
        }),
        [](const PropertyList& p) -> ReturnValue {
            std::string action = p["action"].value<std::string>();
            if (action == "close") {
                ShowLivePreview(false);
                return std::string("摄像头已关闭");
            }
            if (action == "status") {
                return CameraWatchOn() ? std::string("摄像头开着，右上角是实时小窗")
                                       : std::string("摄像头关着");
            }
            ShowLivePreview(true);
            return CameraWatchOn() ? std::string("实时画面已打开：七宝在大画面，摄像头在右上角小窗")
                                   : std::string("实时画面没打开，预览任务没有起来");
        });
    ESP_LOGI(TAG, "实时摄像头工具已注册：self.qibao.camera");
}

}  // namespace qibao

// self.camera.take_photo 讲解完这一帧之后调用。
// 打开的是和屏幕按钮同一条实时小窗，不再铺半屏静帧。
void QibaoOnPhotoTool() {
    qibao::ShowLivePreview(true);
}

// ---- 给 application.cc 的 C 弱符号实现 ----
extern "C" bool QibaoGateAllowed(const char* command) {
    (void)command;
    // 没录入主人人脸 = 功能降级模式，放行（避免把自己锁在外面）
    if (!qibao::HasOwnerFace()) return true;
    // 已解锁（10 分钟内）直接放行
    if (qibao::IsUnlocked()) return true;
    // 未解锁只拒绝本机控制指令，不要在语音会话里启动摄像头+人脸模型。
    // 那条推理和屏幕/音频抢 cache，会把正在进行的小智对话直接重启掉。
    // 想解锁用串口 qface（空闲时刷一次）。
    ESP_LOGW(TAG, "[Qibao] 未解锁，拒绝控制指令（不在对话中启动刷脸）");
    return false;
}

extern "C" void QibaoOnWake() {
    // 唤醒瞬间不要刷脸。语音通道要先连上、先能对话。
    ESP_LOGI(TAG, "唤醒：先进行语音对话，本次不跑刷脸");
}
