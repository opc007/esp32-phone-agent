#include "portal_client.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <esp_log.h>
#include <esp_http_client.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cJSON.h>

#define TAG "Portal"

namespace {

constexpr const char* kNvsNs = "portal";
constexpr size_t kMaxBody = 160 * 1024;   // a11y 树可能很大；超了直接截断，避免吃满内存

// 把 {"status":"success","result":...} 解包成 result 本身
bool Unwrap(std::string& payload, std::string& err) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) return true;   // 非 JSON，原样返回
    cJSON* st = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(st) && std::strcmp(st->valuestring, "error") == 0) {
        // Portal 的错误响应是 {"status":"error","error":"..."}
        cJSON* msg = cJSON_GetObjectItem(root, "error");
        if (!cJSON_IsString(msg)) msg = cJSON_GetObjectItem(root, "message");
        err = cJSON_IsString(msg) ? msg->valuestring : "Portal 返回错误";
        cJSON_Delete(root);
        return false;
    }
    cJSON* res = cJSON_GetObjectItem(root, "result");
    if (!res) res = cJSON_GetObjectItem(root, "data");
    if (res) {
        if (cJSON_IsString(res)) {
            payload = res->valuestring;
        } else {
            char* s = cJSON_PrintUnformatted(res);
            if (s) { payload = s; cJSON_free(s); }
        }
    }
    cJSON_Delete(root);
    return true;
}

std::string ToJsonEscaped(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else out.push_back(c);
    }
    return out;
}

}  // namespace

PortalClient& PortalClient::GetInstance() {
    static PortalClient instance;
    return instance;
}

void PortalClient::SetError(const std::string& msg) {
    last_error_ = msg;
    ESP_LOGW(TAG, "%s", msg.c_str());
}

void PortalClient::Set(const std::string& host, const std::string& token) {
    host_ = host;
    token_ = token;
    last_error_.clear();
}

void PortalClient::Load() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;
    char buf[256];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, "host", buf, &len) == ESP_OK) host_ = buf;
    len = sizeof(buf);
    if (nvs_get_str(h, "token", buf, &len) == ESP_OK) token_ = buf;
    nvs_close(h);
    if (!host_.empty()) ESP_LOGI(TAG, "Portal 配置已载入: %s", host_.c_str());
}

bool PortalClient::Save(const std::string& host, const std::string& token) {
    // 内存值立即生效；NVS 只是持久化，失败不影响本次会话。
    host_ = host;
    token_ = token;
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "host", host.c_str());
    nvs_set_str(h, "token", token.c_str());
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    ESP_LOGI(TAG, "Portal 配置已保存: %s", host_.c_str());
    return true;
}

std::string PortalClient::BaseUrl(const std::string& path) const {
    std::string base = host_;
    if (base.find("://") == std::string::npos) {
        base = "http://" + base;
    }
    // 没写端口就补 8080（只看 host 部分是否有冒号）
    size_t scheme = base.find("://");
    std::string after = base.substr(scheme + 3);
    if (after.find(':') == std::string::npos) {
        base += ":8080";
    }
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

bool PortalClient::Request(bool post, const std::string& path,
                           const std::string& body, std::string& out,
                           int timeout_ms) {
    if (host_.empty()) {
        SetError("尚未配置手机地址（先对我说：设置手机地址 <IP> 令牌 <token>）");
        return false;
    }
    std::string url = BaseUrl(path);
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = timeout_ms;
    // 小缓冲：这台设备内部 SRAM 要留给音频 UDP 通道；HTTP 请求/响应都很短。
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    cfg.disable_auto_redirect = true;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { SetError("HTTP 初始化失败"); return false; }

    if (!token_.empty()) {
        std::string auth = "Bearer " + token_;
        esp_http_client_set_header(cli, "Authorization", auth.c_str());
    }
    if (post) {
        // 必须显式设置方法：set_post_field() 只填 body，用 esp_http_client_open()
        // 时默认仍是 GET，服务端会返回 404（所有点击/输入/启动应用静默失效）。
        esp_http_client_set_method(cli, HTTP_METHOD_POST);
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_post_field(cli, body.c_str(), (int)body.size());
    }

    out.clear();
    esp_err_t err = esp_http_client_open(cli, post ? (int)body.size() : 0);
    if (err == ESP_OK && post && !body.empty()) {
        int written = esp_http_client_write(cli, body.c_str(), (int)body.size());
        if (written != (int)body.size()) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        // 重要：这里原来是 char buf[2048] 的栈缓冲。串口控制台任务栈只有 4KB，
        // 一进入 HTTP 读就直接爆栈，表现为 InstrFetchProhibited / PC=0 / 自动重启。
        // 改到堆上，让小栈任务也能安全调用 Portal。
        std::vector<char> buf(1024);
        int r;
        while ((r = esp_http_client_read(cli, buf.data(), (int)buf.size())) > 0) {
            out.append(buf.data(), r);
            if (out.size() > kMaxBody) break;
        }
    }
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK && out.empty()) {
        SetError("连接失败: " + std::string(esp_err_to_name(err)) + " (" + url + ")");
        return false;
    }
    if (status == 401 || status == 403) {
        SetError("鉴权失败（令牌可能已变，请在 App 里重新获取）");
        return false;
    }
    if (status != 200) {
        SetError("HTTP " + std::to_string(status));
        return false;
    }
    std::string perr;
    if (!Unwrap(out, perr)) {
        SetError(perr);
        return false;
    }
    return true;
}

bool PortalClient::Get(const std::string& path, std::string& out, int timeout_ms) {
    return Request(false, path, "", out, timeout_ms);
}

bool PortalClient::Post(const std::string& path, const std::string& json,
                        std::string& out, int timeout_ms) {
    return Request(true, path, json, out, timeout_ms);
}

bool PortalClient::GetState(std::string& out_json) {
    return Get("/state", out_json, 12000);
}

bool PortalClient::Tap(int x, int y) {
    std::string out;
    std::string body = "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}";
    return Post("/tap", body, out, 6000);
}

bool PortalClient::Swipe(int x1, int y1, int x2, int y2, int duration_ms) {
    std::string out;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"startX\":%d,\"startY\":%d,\"endX\":%d,\"endY\":%d,\"duration\":%d}",
             x1, y1, x2, y2, duration_ms);
    return Post("/swipe", buf, out, 8000);
}

bool PortalClient::InputText(const std::string& utf8, bool clear) {
    // base64 编码（Portal 要求）
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    const unsigned char* p = (const unsigned char*)utf8.data();
    size_t n = utf8.size();
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        b64.push_back(kTable[(v >> 18) & 63]);
        b64.push_back(kTable[(v >> 12) & 63]);
        b64.push_back(i + 1 < n ? kTable[(v >> 6) & 63] : '=');
        b64.push_back(i + 2 < n ? kTable[v & 63] : '=');
    }
    std::string out;
    std::string body = "{\"base64_text\":\"" + b64 + "\",\"clear\":" +
                       (clear ? "true" : "false") + "}";
    return Post("/keyboard/input", body, out, 10000);
}

bool PortalClient::SendKey(int android_keycode) {
    std::string out;
    std::string body = "{\"key_code\":" + std::to_string(android_keycode) + "}";
    return Post("/keyboard/key", body, out, 6000);
}

bool PortalClient::StartApp(const std::string& package) {
    std::string out;
    std::string body = "{\"package\":\"" + ToJsonEscaped(package) + "\"}";
    return Post("/app", body, out, 10000);
}
