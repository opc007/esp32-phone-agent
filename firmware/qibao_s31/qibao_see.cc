#include "qibao_see.h"

#include "qibao_persona.h"
#include "qibao_sdcard.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <driver/uart.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "coco_detect.hpp"
#include "dl_image_define.hpp"

namespace qibao {
namespace {

const char* TAG = "QibaoSee";
const char* kModelUrl =
    "https://raw.githubusercontent.com/espressif/esp-dl/"
    "eb72563e6a4e61d2bbc0d46e4c79f2c9f816c3c6/models/coco_detect/models/p4/"
    "coco_detect_yolo11n_320_s8_v1.espdl";
const char* kModelPath = "/sdcard/qibao/coco/coco_detect_yolo11n_320_s8_v1.espdl";

const char* kCocoZh[] = {
    "人",     "自行车", "汽车",   "摩托车", "飞机",   "公交车", "火车",   "卡车",
    "船",     "红绿灯", "消防栓", "停车标志", "停车计时器", "长椅", "鸟", "猫",
    "狗",     "马",     "羊",     "牛",     "大象",   "熊",     "斑马",   "长颈鹿",
    "背包",   "雨伞",   "手提包", "领带",   "行李箱", "飞盘",   "滑雪板", "单板滑雪",
    "球",     "风筝",   "棒球棒", "棒球手套", "滑板", "冲浪板", "网球拍", "瓶子",
    "酒杯",   "杯子",   "叉子",   "刀",     "勺子",   "碗",     "香蕉",   "苹果",
    "三明治", "橙子",   "西兰花", "胡萝卜", "热狗",   "披萨",   "甜甜圈", "蛋糕",
    "椅子",   "沙发",   "盆栽",   "床",     "餐桌",   "马桶",   "电视",   "笔记本",
    "鼠标",   "遥控器", "键盘",   "手机",   "微波炉", "烤箱",   "烤面包机", "水槽",
    "冰箱",   "书",     "时钟",   "花瓶",   "剪刀",   "泰迪熊", "吹风机", "牙刷",
};

COCODetect* g_coco = nullptr;
bool g_load_skipped = false;
char g_badge[32] = {};
std::string g_last;

bool FileOk() {
    struct stat st {};
    return stat(kModelPath, &st) == 0 && st.st_size > 1000000;
}

bool DownloadModel() {
    if (!qibao_sd::Init() || !qibao_sd::Available()) {
        ESP_LOGE(TAG, "TF 卡不可用，物品模型没法保存");
        return false;
    }
    mkdir("/sdcard/qibao", 0775);
    mkdir("/sdcard/qibao/coco", 0775);
    FILE* fp = fopen(kModelPath, "wb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "打不开 %s", kModelPath);
        return false;
    }
    struct Dl {
        FILE* fp;
        int total;
    } dl{fp, 0};
    esp_http_client_config_t cfg = {};
    cfg.url = kModelUrl;
    cfg.timeout_ms = 300000;
    cfg.buffer_size = 4096;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.max_redirection_count = 8;
    cfg.user_data = &dl;
    cfg.event_handler = [](esp_http_client_event_t* evt) {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr && evt->data_len > 0) {
            auto* dl = static_cast<Dl*>(evt->user_data);
            fwrite(evt->data, 1, evt->data_len, dl->fp);
            dl->total += evt->data_len;
        }
        return ESP_OK;
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = client == nullptr ? ESP_FAIL : esp_http_client_perform(client);
    int status = client == nullptr ? 0 : esp_http_client_get_status_code(client);
    fclose(fp);
    if (client != nullptr) esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "物品模型下载 %d 字节，HTTP %d，%s", dl.total, status, esp_err_to_name(err));
    if (err != ESP_OK || status != 200 || dl.total < 1000000) {
        remove(kModelPath);
        return false;
    }
    return true;
}

bool EnsureCocoBody(bool allow_download) {
    if (g_coco != nullptr) return true;
    if (!FileOk()) {
        if (!allow_download) return false;
        ESP_LOGI(TAG, "TF 卡上没有物品模型，开始下载 S31 用的 p4 版 YOLO11n 320");
        if (!DownloadModel()) return false;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "加载物品模型前，PSRAM 最大空闲块 %u，内部内存 %u",
             (unsigned)largest, (unsigned)internal);
    // 实测：卡上的 2.9MB 模型在内部内存约 95KB 时加载，会把内存打到十几字节，
    // Wi-Fi 定时器分配失败然后重启。宁可这次不认物。
    if (internal < 160 * 1024 || largest < 6 * 1024 * 1024) {
        if (!g_load_skipped) {
            ESP_LOGW(TAG, "内存不够安全加载物品模型，跳过");
            g_load_skipped = true;
        }
        return false;
    }
    g_coco = new COCODetect(COCODetect::YOLO11N_320_S8_V1, false);
    if (g_coco == nullptr || g_coco->get_raw_model() == nullptr) {
        ESP_LOGE(TAG, "物品模型没有加载起来");
        delete g_coco;
        g_coco = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "物品模型就绪，内部内存 %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

bool EnsureCoco(bool allow_download) {
    if (g_coco != nullptr) return true;
    if (!allow_download && (!FileOk() || g_load_skipped)) return false;
    struct Job {
        bool ok;
        bool allow_download;
        TaskHandle_t waiter;
    } job{false, allow_download, xTaskGetCurrentTaskHandle()};
    // 下载和加载要更大的栈。串口任务只有 4KB，在里面做会栈保护重启。
    if (xTaskCreate([](void* arg) {
            auto* job = static_cast<Job*>(arg);
            job->ok = EnsureCocoBody(job->allow_download);
            xTaskNotifyGive(job->waiter);
            vTaskDelete(nullptr);
        }, "qibao_coco", 24576, &job, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "物品模型任务没起来");
        return false;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(180000));
    return job.ok && g_coco != nullptr;
}

}  // namespace

std::string SeeFrame(uint8_t* rgb, int w, int h) {
    if (rgb == nullptr || w <= 0 || h <= 0) return "没有画面";
    if (!EnsureCoco(false)) return "物品模型还没准备好";
    dl::image::img_t image = {};
    image.data = rgb;
    image.width = (uint16_t)w;
    image.height = (uint16_t)h;
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    auto& found = g_coco->run(image);
    g_last.clear();
    g_badge[0] = '\0';
    int shown = 0;
    for (auto& item : found) {
        if (item.score < 0.35f) continue;
        const char* name = "未知物品";
        if (item.category >= 0 &&
            item.category < (int)(sizeof(kCocoZh) / sizeof(kCocoZh[0]))) {
            name = kCocoZh[item.category];
        }
        if (shown == 0) {
            std::snprintf(g_badge, sizeof(g_badge), "%s", name);
        }
        if (shown > 0) g_last += "、";
        g_last += name;
        ESP_LOGI(TAG, "看到 %s %.2f", name, item.score);
        if (++shown >= 3) break;
    }
    if (g_last.empty()) {
        ESP_LOGI(TAG, "这帧没有认出物品");
        return "这帧没有认出物品";
    }
    return std::string("看到了：") + g_last;
}

bool ObjectModelOnDisk() { return FileOk(); }

void TryPreloadObjects() {
    if (g_coco != nullptr || g_load_skipped || !FileOk()) return;
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (internal < 180 * 1024 || largest < 6 * 1024 * 1024) {
        ESP_LOGW(TAG, "开机内存不够预加载物品模型（内部 %u，PSRAM 块 %u），等显式 qobj",
                 (unsigned)internal, (unsigned)largest);
        g_load_skipped = true;
        return;
    }
    ESP_LOGI(TAG, "开机内存还宽裕，预加载物品模型");
    EnsureCoco(false);
}

int ReceiveModel(int expect) {
    if (expect < 1000000 || expect > 4000000) return -1;
    if (!qibao_sd::Init() || !qibao_sd::Available()) {
        ESP_LOGE(TAG, "TF 卡不可用");
        return -1;
    }
    mkdir("/sdcard/qibao", 0775);
    mkdir("/sdcard/qibao/coco", 0775);
    FILE* fp = fopen(kModelPath, "wb");
    if (fp == nullptr) return -1;
    // 控制台原来的接收环形缓冲只有 256 字节。写卡一慢，主机继续发就会丢字节。
    uart_driver_delete(UART_NUM_0);
    uart_driver_install(UART_NUM_0, 32768, 0, 0, nullptr, 0);
    ESP_LOGI(TAG, "MODEL READY %d", expect);
    int got = 0;
    uint8_t buf[4096];
    int64_t last = esp_timer_get_time();
    while (got < expect) {
        int chunk = expect - got;
        if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);
        int n = uart_read_bytes(UART_NUM_0, buf, chunk, pdMS_TO_TICKS(200));
        if (n > 0) {
            fwrite(buf, 1, n, fp);
            got += n;
            last = esp_timer_get_time();
        } else if (esp_timer_get_time() - last > 20000000) {
            break;
        }
    }
    fclose(fp);
    if (got != expect) {
        remove(kModelPath);
        ESP_LOGE(TAG, "模型没收全，%d/%d", got, expect);
        return got;
    }
    ESP_LOGI(TAG, "模型已写入 TF 卡 %d 字节", got);
    return got;
}

std::string SeeOnce() {
    int w = 0, h = 0;
    uint8_t* rgb = GrabRgb565(&w, &h);
    if (rgb == nullptr) return "没有抓到摄像头画面";
    if (!EnsureCoco(true)) {
        heap_caps_free(rgb);
        if (FileOk()) {
            return "物品模型已在 TF 卡，但内部内存不够安全加载，这次不跑，避免语音重启";
        }
        return "物品模型没下载成功，可以再敲一次 qobj";
    }
    std::string text = SeeFrame(rgb, w, h);
    heap_caps_free(rgb);
    return text;
}

const char* ObjectBadge() { return g_badge; }

std::string LastObjects() { return g_last; }

}  // namespace qibao
