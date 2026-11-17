#include "danmaku_tools.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "application.h"
#include "camera.h"
#include "esp_camera.h"
#include "mcp_server.h"

#define TAG "Danmaku"

namespace {

// 去重池上限：记住最近 N 条弹幕，超出就丢最老的。
constexpr size_t kMaxSeen = 300;

std::vector<std::string> g_seen;
SemaphoreHandle_t g_lock = nullptr;
bool g_console_busy = false;
// 由板级构造函数显式注入；绝不在构造期间调 Board::GetInstance()（会死锁）。
Camera* g_camera = nullptr;

SemaphoreHandle_t Lock() {
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutex();
    }
    return g_lock;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

Camera* Cam() {
    return g_camera;
}

constexpr const char* kDanmakuPrompt =
    "这张照片拍的是一块屏幕（电脑或手机上的直播画面）。"
    "请把画面中所有可见的弹幕、评论文字逐条提取出来，"
    "只输出一个JSON字符串数组，例如[\"666\",\"主播好漂亮\"]。"
    "画面里完全没有弹幕或评论时只输出[]。"
    "不要输出数组以外的任何解释文字。";

// 拍一张照片并让云端视觉回答 question；失败时 err 非空。
// 与上游 self.camera.take_photo 相同做法：拍照前降低当前任务优先级，
// 结束后恢复（TaskPriorityReset RAII）。
std::string CaptureAndExplain(const char* question, std::string* err) {
    err->clear();
    auto* cam = Cam();
    if (cam == nullptr) {
        *err = "设备没有初始化摄像头";
        return {};
    }
    TaskPriorityReset priority_reset(1);
    if (!cam->Capture()) {
        *err = "拍照失败（检查摄像头排线/镜头遮挡）";
        return {};
    }
    auto ans = cam->Explain(question);
    if (!ans) {
        *err = ans.error();
        return {};
    }
    return *ans;
}

// 视觉回复可能是裸数组、也可能裹在 ```json ... ``` 里，取第一个 [ 到最后一个 ]。
std::vector<std::string> ParseDanmakuArray(const std::string& s) {
    std::vector<std::string> out;
    auto b = s.find('[');
    auto e = s.rfind(']');
    if (b == std::string::npos || e == std::string::npos || e <= b) {
        return out;
    }
    cJSON* arr = cJSON_Parse(s.substr(b, e - b + 1).c_str());
    if (arr == nullptr || !cJSON_IsArray(arr)) {
        if (arr != nullptr) cJSON_Delete(arr);
        return out;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && item->valuestring != nullptr &&
            item->valuestring[0] != '\0') {
            out.push_back(item->valuestring);
        }
    }
    cJSON_Delete(arr);
    return out;
}

bool HasSeen(const std::string& text) {
    for (const auto& s : g_seen) {
        if (s == text) return true;
    }
    return false;
}

// 调用方必须已持有 g_lock。
std::string ReadNewDanmakuLocked() {
    std::string err;
    std::string ans = CaptureAndExplain(kDanmakuPrompt, &err);
    if (!err.empty()) {
        return std::string("{\"status\":\"error\",\"error\":\"") + JsonEscape(err) +
               "\",\"hint\":\"如果提示 explain URL 未设置，说明小智控制台还没有给"
               "这个智能体绑定『视觉理解』服务，请让用户到控制台绑定后再试\"}";
    }
    auto items = ParseDanmakuArray(ans);
    std::vector<std::string> fresh;
    for (const auto& it : items) {
        if (!HasSeen(it)) {
            fresh.push_back(it);
        }
    }
    for (const auto& it : items) {
        if (g_seen.size() >= kMaxSeen) {
            g_seen.erase(g_seen.begin());
        }
        g_seen.push_back(it);
    }
    std::string out = "{\"status\":\"ok\",\"new_count\":" +
                      std::to_string(fresh.size()) + ",\"screen_count\":" +
                      std::to_string(items.size()) + ",\"new\":[";
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (i > 0) out += ",";
        out += "\"" + JsonEscape(fresh[i]) + "\"";
    }
    out += "]}";
    return out;
}

std::string ReadNewDanmaku() {
    std::string result;
    if (xSemaphoreTake(Lock(), portMAX_DELAY) == pdTRUE) {
        result = ReadNewDanmakuLocked();
        xSemaphoreGive(Lock());
    } else {
        result = "{\"status\":\"error\",\"error\":\"busy\"}";
    }
    return result;
}

std::string ResetSeen() {
    if (xSemaphoreTake(Lock(), portMAX_DELAY) == pdTRUE) {
        g_seen.clear();
        xSemaphoreGive(Lock());
    }
    return "{\"status\":\"ok\",\"cleared\":true}";
}

}  // namespace

void StartDanmakuTools(Camera* camera) {
    g_camera = camera;
    if (g_camera == nullptr) {
        ESP_LOGW(TAG, "没有摄像头，弹幕工具不注册");
        return;
    }
    Lock();
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.danmaku.read",
        "读取设备摄像头画面中「新增」的直播弹幕/评论文字。"
        "设备会拍一张当前画面（摄像头必须对着手机或电脑的直播屏幕），"
        "由云端视觉识别出画面里所有弹幕文字，并与上次对比、只返回新出现的弹幕；"
        "相同文字短时间重复出现只报一次。\n"
        "用户说「看看弹幕」「读读弹幕」「有人发弹幕就告诉我」「帮我盯着屏幕/直播间」"
        "时，必须调用本工具，不要回答看不到。\n"
        "要持续监听播报弹幕时：反复调用本工具，两次调用之间隔约10秒，"
        "直到用户叫停；返回 new 为空数组表示这段时间没有新弹幕，可继续调用。\n"
        "回答用户时只依据返回的真实弹幕内容，不要编造弹幕。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return ReadNewDanmaku();
        });

    mcp.AddTool(
        "self.danmaku.reset",
        "清空弹幕去重记录，已播报过的弹幕会被忘掉。"
        "用户切换直播间/换了屏幕画面，或说「重新开始监听弹幕」时调用一次，"
        "然后继续用 self.danmaku.read 监听。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return ResetSeen();
        });

    ESP_LOGI(TAG, "弹幕监听工具已注册: self.danmaku.read / self.danmaku.reset");
}

namespace {

struct ConsoleJob {
    bool list;  // true=读弹幕 false=拍照描述
    std::string question;
};

// 在线切分辨率后试拍一张，用于免重编译排查「取帧超时」。
void CamTest(const std::string& fs_name) {
    sensor_t* s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGW(TAG, "[camtest] 摄像头驱动未初始化");
        return;
    }
    framesize_t fs = FRAMESIZE_INVALID;
    if (fs_name == "qqvga") fs = FRAMESIZE_QQVGA;
    else if (fs_name == "qvga") fs = FRAMESIZE_QVGA;
    else if (fs_name == "vga") fs = FRAMESIZE_VGA;
    else if (fs_name == "svga") fs = FRAMESIZE_SVGA;
    else if (fs_name == "hd") fs = FRAMESIZE_HD;
    else if (fs_name == "sxga") fs = FRAMESIZE_SXGA;
    else if (fs_name == "uxga") fs = FRAMESIZE_UXGA;
    // 可选 jpg 前缀：让传感器直接输出 JPEG（排查 RGB565 模式不出帧）
    bool jpeg = false;
    if (fs_name == "jpgqqvga") { fs = FRAMESIZE_QQVGA; jpeg = true; }
    else if (fs_name == "jpgvga") { fs = FRAMESIZE_VGA; jpeg = true; }
    else if (fs_name == "jpghd") { fs = FRAMESIZE_HD; jpeg = true; }
    if (fs == FRAMESIZE_INVALID) {
        ESP_LOGW(TAG, "[camtest] 未知分辨率 %s（支持 qqvga/qvga/vga/svga/hd/sxga/uxga 或 jpg 前缀）",
                 fs_name.c_str());
        return;
    }
    int64_t t0 = esp_timer_get_time();
    if (jpeg) {
        s->set_pixformat(s, PIXFORMAT_JPEG);
    } else {
        s->set_pixformat(s, PIXFORMAT_RGB565);
    }
    s->set_framesize(s, fs);
    // 连拍两张丢掉第一张（切换后的旧帧）
    camera_fb_t* fb1 = esp_camera_fb_get();
    if (fb1 == nullptr) {
        ESP_LOGW(TAG, "[camtest] %s 第1帧取帧失败(%.1fs)", fs_name.c_str(),
                 (esp_timer_get_time() - t0) / 1000.0);
        return;
    }
    esp_camera_fb_return(fb1);
    camera_fb_t* fb2 = esp_camera_fb_get();
    if (fb2 == nullptr) {
        ESP_LOGW(TAG, "[camtest] %s 第2帧取帧失败", fs_name.c_str());
        return;
    }
    ESP_LOGI(TAG, "[camtest] %s 成功: %dx%d len=%u fmt=%d 耗时%.1fs", fs_name.c_str(),
             fb2->width, fb2->height, (unsigned)fb2->len, fb2->format,
             (esp_timer_get_time() - t0) / 1000.0);
    esp_camera_fb_return(fb2);
}

}  // namespace

std::string DanmakuConsoleCommand(const std::string& cmd,
                                  const std::vector<std::string>& args) {
    if (cmd == "camtest") {
        if (args.empty()) return "用法: camtest <qqvga|qvga|vga|svga|hd|sxga|uxga>";
        if (g_console_busy) return "上一个拍照任务还在跑，稍等";
        auto* job = new ConsoleJob();
        job->list = false;
        job->question = args[0];
        g_console_busy = true;
        BaseType_t ok = xTaskCreate(
            [](void* p) {
                auto* job = static_cast<ConsoleJob*>(p);
                CamTest(job->question);
                delete job;
                g_console_busy = false;
                vTaskDelete(nullptr);
            },
            "cam_test", 8192, job, 3, nullptr);
        if (ok != pdPASS) {
            delete job;
            g_console_busy = false;
            return "创建测试任务失败";
        }
        return "已后台切换分辨率试拍，看 [camtest] 日志";
    }
    if (cmd == "macdanmakureset") {
        ResetSeen();
        return "已清空弹幕去重记录";
    }
    if (cmd == "macphoto" || cmd == "macdanmaku") {
        // 拍照+云端视觉是重活（JPEG 编码起 std::thread、HTTP 上传），
        // 串口自测任务只有 4KB 栈，必须丢到 12KB 大栈任务里跑，否则爆栈重启。
        if (g_console_busy) {
            return "上一个拍照任务还在跑，稍等";
        }
        auto* job = new ConsoleJob();
        job->list = (cmd == "macdanmaku");
        for (const auto& a : args) {
            if (!job->question.empty()) job->question += " ";
            job->question += a;
        }
        if (job->question.empty()) {
            job->question = "用两三句话简要描述这张照片的内容，画面里有文字的话把文字也读出来。";
        }
        g_console_busy = true;
        BaseType_t ok = xTaskCreate(
            [](void* p) {
                auto* job = static_cast<ConsoleJob*>(p);
                if (job->list) {
                    ESP_LOGI(TAG, "[自测] 开始拍照读弹幕...");
                    std::string r = ReadNewDanmaku();
                    ESP_LOGI(TAG, "[弹幕] %s", r.c_str());
                } else {
                    ESP_LOGI(TAG, "[自测] 开始拍照...");
                    std::string err;
                    std::string ans = CaptureAndExplain(job->question.c_str(), &err);
                    if (!err.empty()) {
                        ESP_LOGW(TAG, "[拍照] 失败: %s", err.c_str());
                    } else {
                        ESP_LOGI(TAG, "[视觉] %s", ans.c_str());
                    }
                }
                delete job;
                g_console_busy = false;
                vTaskDelete(nullptr);
            },
            "danmaku_test", 12288, job, 3, nullptr);
        if (ok != pdPASS) {
            delete job;
            g_console_busy = false;
            return "创建拍照任务失败";
        }
        return job->list ? "已后台读弹幕，看 [弹幕] 日志"
                         : "已后台拍照，看 [拍照]/[视觉] 日志";
    }
    return "";
}
