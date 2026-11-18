/*
 * qibao_sdcard.cc —— 七宝妹妹 TF 卡存储（ESP32-S31-Korvo-1）
 *
 * 板载 microSD 走 SDMMC 4bit（官方 BSP 引脚）：
 *   CLK=GPIO24  CMD=GPIO25  D0=GPIO20  D1=GPIO21  D2=GPIO22  D3=GPIO23
 * 电源控制 SD_CTRL=GPIO39（高电平使能）。
 * 挂载 FATFS 到 /sdcard，人脸特征存 /sdcard/qibao/owner.feat。
 */
#include "qibao_sdcard.h"

#include <esp_log.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static const char* TAG = "QibaoSD";

namespace qibao_sd {

static bool s_available = false;
static bool s_init_done = false;
static sdmmc_card_t* s_card = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;

// 官方 BSP：S31 Korvo-1 microSD 4bit 引脚
#define QB_SD_PIN_CLK   GPIO_NUM_24
#define QB_SD_PIN_CMD   GPIO_NUM_25
#define QB_SD_PIN_D0    GPIO_NUM_20
#define QB_SD_PIN_D1    GPIO_NUM_21
#define QB_SD_PIN_D2    GPIO_NUM_22
#define QB_SD_PIN_D3    GPIO_NUM_23
// 注意：官方 S31-Korvo SD 范例没有任何电源控制引脚（直接供电），
// 因此不操作 GPIO39（那可能是 SD 检测/NAND 切换脚，拉高反而干扰）。

#define QB_FEATURE_PATH "/sdcard/qibao/owner.feat"

bool Init() {
    if (s_init_done) return s_available;
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_init_done) {  // 双重检查
        xSemaphoreGive(s_mutex);
        return s_available;
    }
    s_init_done = true;

    vTaskDelay(pdMS_TO_TICKS(200));  // 等卡上电稳定

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_PROBING;  // 探测阶段用最低频(400kHz)，稳

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = QB_SD_PIN_CLK;
    slot.cmd = QB_SD_PIN_CMD;
    slot.d0 = QB_SD_PIN_D0;
    slot.d1 = QB_SD_PIN_D1;
    slot.d2 = QB_SD_PIN_D2;
    slot.d3 = QB_SD_PIN_D3;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF 卡挂载失败（%s），人脸特征将回退存 NVS", esp_err_to_name(ret));
        s_available = false;
    } else {
        s_available = true;
        ESP_LOGI(TAG, "TF 卡挂载成功：%s（%d kHz, 4-bit）", s_card->cid.name,
                 s_card->max_freq_khz);
        // 创建 /sdcard/qibao 目录（mkdir 已存在会返回 EEXIST，忽略）
        FILE* d = fopen("/sdcard/qibao", "r");
        if (d == nullptr) {
            ::mkdir("/sdcard/qibao", 0777);
        } else {
            fclose(d);
        }
        // 读写自检：写一个测试文件再读回，确认 TF 卡不只是挂载、而是真能存人脸特征
        FILE* t = fopen("/sdcard/qibao/.write_test", "wb");
        if (t != nullptr) {
            const char* magic = "qibao-sd-ok";
            size_t nw = fwrite(magic, 1, strlen(magic), t);
            fclose(t);
            FILE* r = fopen("/sdcard/qibao/.write_test", "rb");
            char buf[32] = {0};
            size_t nr = r != nullptr ? fread(buf, 1, sizeof(buf) - 1, r) : 0;
            if (r != nullptr) fclose(r);
            ESP_LOGI(TAG, "TF 卡读写自检：%s（写 %d / 读 %d 字节）",
                     (nw == strlen(magic) && nr == strlen(magic) && strcmp(buf, magic) == 0)
                         ? "通过" : "失败", (int)nw, (int)nr);
        } else {
            ESP_LOGE(TAG, "TF 卡读写自检失败：无法创建测试文件");
        }
    }
    xSemaphoreGive(s_mutex);
    return s_available;
}

bool Available() {
    Init();
    return s_available;
}

bool SaveFeature(const float* feat, int len) {
    if (!Init() || feat == nullptr || len <= 0 || len > kMaxFeatureLen) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = false;
    FILE* f = fopen(QB_FEATURE_PATH, "wb");
    if (f != nullptr) {
        size_t n = fwrite(feat, sizeof(float), (size_t)len, f);
        ok = (n == (size_t)len);
        fclose(f);
        if (ok) ESP_LOGI(TAG, "人脸特征已存 TF 卡：%d 维", len);
    } else {
        ESP_LOGE(TAG, "无法写入 TF 卡特征文件");
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

bool LoadFeature(float* out, int* len) {
    if (!Init() || out == nullptr || len == nullptr) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = false;
    FILE* f = fopen(QB_FEATURE_PATH, "rb");
    if (f != nullptr) {
        size_t n = fread(out, sizeof(float), (size_t)kMaxFeatureLen, f);
        fclose(f);
        if (n > 0 && n <= (size_t)kMaxFeatureLen) {
            *len = (int)n;
            ok = true;
            ESP_LOGI(TAG, "从 TF 卡载入人脸特征：%d 维", *len);
        }
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

void ClearFeature() {
    if (!Init()) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (remove(QB_FEATURE_PATH) == 0) {
        ESP_LOGI(TAG, "已删除 TF 卡人脸特征");
    }
    xSemaphoreGive(s_mutex);
}

}  // namespace qibao_sd
