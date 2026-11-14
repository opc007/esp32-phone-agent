/*
 * phone_bridge.cc —— ESP32 自托管「直连手机」通道
 *
 * 目标：链路里没有电脑。手机只需装一个 19KB 的无障碍 APK（orb-eye, MIT），
 * 打开系统里那一个开关；APK 由 ESP32 自己的 HTTP 引导页托管。ESP32 通过
 * WiFi 直接访问手机 :7333 的 /screen /click /setText /swipe 等接口，
 * 中文整句编排、控件树解析、二次确认全部在固件里完成，云端小智只管对话。
 *
 * 为什么 APK 不能由 ESP32 静默安装：Android 不允许任何外部设备代替用户
 * 授予「安装未知应用」或「无障碍服务」权限，这是系统安全边界，也是本项目
 * 明确不尝试绕过的红线。ESP32 能做的是把引导/下载/发现/绑定全部自动化。
 */
#include "phone_bridge.h"
#include "orb_eye_apk.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include <cerrno>

#define TAG "PhoneBridge"

namespace {

constexpr int kEyePort = 7333;
constexpr size_t kMaxEyeOut = 24576;
constexpr const char* kNvsNs = "phonebr";
constexpr const char* kNvsKey = "ip";

SemaphoreHandle_t g_lock = nullptr;
std::string g_host;          // 已连接的手机 "192.168.x.x:7333"
int g_last_clen = -1;        // 最近一次 Eye 响应的 Content-Length（诊断用）
std::string g_candidate;     // 引导页来访者 IP（自动学习）
std::string g_saved_ip;      // NVS 里上次的手机 IP
bool g_active = false;
bool g_started = false;
uint32_t g_last_ok_ms = 0;

std::string g_pending_text;   // 待二次确认的敏感动作
std::string g_pending_token;
std::string g_last_key, g_last_result;
uint32_t g_last_key_ms = 0;

httpd_handle_t g_server = nullptr;
bool g_httpd_started = false;

struct Guard {
    Guard() { if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Guard() { if (g_lock) xSemaphoreGive(g_lock); }
};

uint32_t NowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n，,。:：");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n，,。:：");
    return s.substr(a, b - a + 1);
}

// 生成合法 JSON 字符串（含两端引号）。中文按 UTF-8 原样保留。
std::string Jq(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    o += '"';
    return o;
}

std::string JStr(const std::string& json, const char* key) {
    cJSON* r = cJSON_Parse(json.c_str());
    if (!r) return "";
    cJSON* v = cJSON_GetObjectItem(r, key);
    std::string o = cJSON_IsString(v) ? v->valuestring : "";
    cJSON_Delete(r);
    return o;
}

int JInt(const std::string& json, const char* key, int def = 0) {
    cJSON* r = cJSON_Parse(json.c_str());
    if (!r) return def;
    cJSON* v = cJSON_GetObjectItem(r, key);
    int o = cJSON_IsNumber(v) ? v->valueint : def;
    cJSON_Delete(r);
    return o;
}

bool EyeError(const std::string& json, std::string& msg) {
    cJSON* r = cJSON_Parse(json.c_str());
    if (!r) return false;
    cJSON* ok = cJSON_GetObjectItem(r, "ok");
    cJSON* e = cJSON_GetObjectItem(r, "error");
    bool bad = (ok && cJSON_IsBool(ok) && !cJSON_IsTrue(ok)) || (e && cJSON_IsString(e));
    if (bad && cJSON_IsString(e)) msg = e->valuestring;
    cJSON_Delete(r);
    return bad;
}

// ---------- NVS：只记住手机 IP，重启后免重新发现 ----------
void LoadSavedIp() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;
    char buf[64] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, kNvsKey, buf, &len) == ESP_OK) g_saved_ip = buf;
    nvs_close(h);
}

void SaveIp(const std::string& ip) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, kNvsKey, ip.c_str());
    nvs_commit(h);
    nvs_close(h);
}

// ---------- 手机端无障碍服务 HTTP 客户端 ----------
// body 为空走 GET，非空走 POST(JSON)。所有动作串行，避免手势互相打断。
bool Eye(const std::string& path, const std::string& body, std::string& out,
         int timeout_ms = 6000) {
    std::string host;
    { Guard g; host = g_host; }
    if (host.empty()) { out = "手机还没接入"; return false; }

    std::string url = "http://" + host + path;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.disable_auto_redirect = true;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { out = "HTTP 客户端初始化失败"; return false; }

    int len = 0;
    esp_http_client_set_header(c, "Connection", "close");
    if (body.empty()) {
        esp_http_client_set_method(c, HTTP_METHOD_GET);
    } else {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body.c_str(), (int)body.size());
        len = (int)body.size();
    }

    out.clear();
    bool ok = false;
    int t0 = (int)(esp_timer_get_time() / 1000);
    int t_open = 0, t_hdr = 0;
    if (esp_http_client_open(c, len) == ESP_OK) {
        t_open = (int)(esp_timer_get_time() / 1000) - t0;
        // 关键：open() 只发请求头；POST body 必须在这里显式写出，
        // 否则 fetch_headers 会死等对端响应直到超时（实测每个 POST 白等 6 秒）。
        if (len > 0 && esp_http_client_write(c, body.c_str(), len) < 0) {
            out = "发送请求体失败";
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return false;
        }
        esp_http_client_fetch_headers(c);
        t_hdr = (int)(esp_timer_get_time() / 1000) - t0;
        // 关键：按 Content-Length 读完就停。orb-eye 的极简 HTTP server 不发
        // `Connection: close`，esp_http_client_read 会一直等到 6 秒超时，
        // 五个动作就会拖过 30 秒工具超时窗口（实测踩过）。
        int content_len = (int)esp_http_client_get_content_length(c);
        char buf[1024];
        int n;
        while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
            out.append(buf, (size_t)n);
            if (out.size() >= kMaxEyeOut) break;
            if (content_len > 0 && (int)out.size() >= content_len) break;
        }
        ok = !out.empty();
        g_last_clen = content_len;
    } else {
        out = "连接手机失败：" + host;
    }
    int t_end = (int)(esp_timer_get_time() / 1000);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    int t_all = (int)(esp_timer_get_time() / 1000) - t0;
    ESP_LOGI(TAG, "[eye] %s body=%dB open=%dms hdr=%dms read=%dms close+cleanup=%dms total=%dms clen=%d",
             path.c_str(), (int)body.size(), t_open, t_hdr, t_end - t0 - t_hdr,
             t_all - (t_end - t0), t_all, g_last_clen);
    return ok;
}

// ---------- 控件树 ----------
struct Node {
    std::string text, desc;
    int l = 0, t = 0, r = 0, b = 0;
    bool clickable = false, editable = false;
};

void ParseBounds(const std::string& s, Node& n) {
    std::string tmp;
    for (char c : s) tmp += (isdigit((unsigned char)c) || c == '-') ? c : ' ';
    int v[4] = {0, 0, 0, 0};
    int got = sscanf(tmp.c_str(), "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
    if (got == 4) { n.l = v[0]; n.t = v[1]; n.r = v[2]; n.b = v[3]; }
}

std::vector<Node> ReadNodes() {
    std::vector<Node> out;
    std::string raw;
    if (!Eye("/screen", "", raw)) return out;
    cJSON* root = cJSON_Parse(raw.c_str());
    if (!root) return out;
    cJSON* els = cJSON_GetObjectItem(root, "elements");
    if (cJSON_IsArray(els)) {
        for (int i = 0; i < cJSON_GetArraySize(els); ++i) {
            cJSON* it = cJSON_GetArrayItem(els, i);
            Node n;
            cJSON* t = cJSON_GetObjectItem(it, "text");
            cJSON* d = cJSON_GetObjectItem(it, "desc");
            cJSON* bd = cJSON_GetObjectItem(it, "bounds");
            cJSON* cl = cJSON_GetObjectItem(it, "clickable");
            cJSON* ed = cJSON_GetObjectItem(it, "editable");
            if (cJSON_IsString(t)) n.text = t->valuestring;
            if (cJSON_IsString(d)) n.desc = d->valuestring;
            if (cJSON_IsString(bd)) ParseBounds(bd->valuestring, n);
            n.clickable = cJSON_IsTrue(cl);
            n.editable = cJSON_IsTrue(ed);
            if (!n.text.empty() || !n.desc.empty()) out.push_back(n);
        }
    }
    cJSON_Delete(root);
    return out;
}

std::string CurrentApp() {
    std::string raw;
    if (!Eye("/info", "", raw)) return "未知";
    std::string w = JStr(raw, "windowPackage");
    if (w.empty()) w = JStr(raw, "package");
    return w.empty() ? "未知" : w;
}

// 把真实控件树压成给云端大模型看的中文简要，绝不编造屏幕内容。
std::string ScreenBrief(size_t limit = 2200) {
    auto nodes = ReadNodes();
    std::string out = "当前应用：" + CurrentApp() + "\n【手机屏幕真实文字】\n";
    std::string prev;
    for (const auto& n : nodes) {
        std::string s = !n.text.empty() ? n.text : n.desc;
        if (s.empty() || s == prev) continue;
        prev = s;
        std::string line = "· " + s;
        if (n.editable) line += " [输入框]";
        else if (n.clickable) line += " [可点]";
        out += line + "\n";
        if (out.size() >= limit) break;
    }
    if (nodes.empty()) out += "（无障碍服务没读到文字，可能是自绘界面或屏幕未解锁）\n";
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::string TapXY(int x, int y) {
    std::string body = "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}";
    std::string out;
    if (!Eye("/tap", body, out)) return "点击失败：" + out;
    std::string e;
    if (EyeError(out, e)) return "点击失败：" + (e.empty() ? out : e);
    return "已点击 (" + std::to_string(x) + "," + std::to_string(y) + ")";
}

// 按界面上真实可见的文字点击；失败时从控件树拿坐标兜底。
std::string ClickText(const std::string& text) {
    std::string body = "{\"text\":" + Jq(text) + "}";
    std::string out;
    if (Eye("/click", body, out)) {
        std::string e;
        if (!EyeError(out, e)) return "已点「" + text + "」";
    }
    for (const auto& n : ReadNodes()) {
        if (n.text.find(text) != std::string::npos || n.desc.find(text) != std::string::npos) {
            int x = (n.l + n.r) / 2, y = (n.t + n.b) / 2;
            if (x > 0 && y > 0) return TapXY(x, y) + "（按屏幕文字「" + n.text + "」定位）";
        }
    }
    return "屏幕上没找到「" + text + "」这个按钮。当前可见：" + ScreenBrief(500);
}

std::string SetText(const std::string& text) {
    std::string body = "{\"text\":" + Jq(text) + "}";
    std::string out;
    if (!Eye("/setText", body, out)) return "输入失败：" + out;
    std::string e;
    if (EyeError(out, e)) {
        // 有些输入框要先点一下才能 SET_TEXT
        for (const auto& n : ReadNodes()) {
            if (n.editable) {
                TapXY((n.l + n.r) / 2, (n.t + n.b) / 2);
                vTaskDelay(pdMS_TO_TICKS(250));
                if (Eye("/setText", body, out) && !EyeError(out, e)) break;
            }
        }
    }
    return EyeError(out, e) ? "输入失败：" + (e.empty() ? out : e)
                            : "已输入「" + text + "」";
}

void ScreenSize(int& w, int& h) {
    w = 1080; h = 2340;
    for (const auto& n : ReadNodes()) {
        if (n.r > w) w = n.r;
        if (n.b > h) h = n.b;
    }
}

std::string SwipeDir(const std::string& dir) {
    int w, h; ScreenSize(w, h);
    int x1, y1, x2, y2;
    if (dir == "down")      { x1 = w/2; y1 = h/4;      x2 = w/2; y2 = h*3/4; }
    else if (dir == "left") { x1 = w*3/4; y1 = h/2;    x2 = w/4; y2 = h/2; }
    else if (dir == "right"){ x1 = w/4; y1 = h/2;      x2 = w*3/4; y2 = h/2; }
    else                    { x1 = w/2; y1 = h*3/4;    x2 = w/2; y2 = h/4; }
    char b[160];
    snprintf(b, sizeof(b),
             "{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"duration\":400}",
             x1, y1, x2, y2);
    std::string out;
    Eye("/swipe", std::string(b), out);
    return "已" + (dir == "up" ? std::string("上滑") : dir == "down" ? std::string("下滑")
                  : dir == "left" ? std::string("左滑") : std::string("右滑"));
}

std::string PressEnter() {
    // 无障碍服务没有 keyevent；按界面上真实存在的「搜索 / 回车 / 确定」按钮。
    // 先读一次控件树、按真实节点点（1 次往返）；找不到再逐个盲试文字点击。
    static const char* kWords[] = {"搜索", "回车", "Enter", "确定", "前往", "发送", "go", "Go"};
    for (const auto& n : ReadNodes()) {
        if (!n.clickable) continue;
        for (const char* k : kWords) {
            if (n.text.find(k) != std::string::npos || n.desc.find(k) != std::string::npos) {
                int x = (n.l + n.r)/2, y = (n.t + n.b)/2;
                if (x > 0 && y > 0)
                    return TapXY(x, y) + "（按屏幕上的「" + (n.text.empty() ? n.desc : n.text) + "」）";
            }
        }
    }
    for (const char* k : kWords) {
        std::string body = "{\"text\":" + Jq(k) + "}";
        std::string out;
        if (Eye("/click", body, out)) {
            std::string e;
            if (!EyeError(out, e)) return std::string("已按「") + k + "」";
        }
    }
    return "屏幕上没找到回车/搜索按钮（自绘键盘时无障碍服务读不到按键）。当前：" + ScreenBrief(400);
}

std::string GlobalAction(const char* which) {
    std::string out;
    Eye(std::string("/") + which, "", out);
    vTaskDelay(pdMS_TO_TICKS(500));
    return std::string("已执行 ") + which;
}

// 打开应用：回到桌面，从桌面真实图标文字里找，绝不写死包名映射表。
// 找不到就翻页再找，仍找不到就如实回报桌面上真实可见的名字。
std::string OpenApp(const std::string& name) {
    if (name.empty()) return "没听清要打开哪个应用。";
    GlobalAction("home");
    vTaskDelay(pdMS_TO_TICKS(650));
    std::vector<std::string> seen;
    for (int page = 0; page < 3; ++page) {
        for (const auto& n : ReadNodes()) {
            const std::string& s = n.text.empty() ? n.desc : n.text;
            if (s.empty()) continue;
            if (s.size() <= 20 && std::find(seen.begin(), seen.end(), s) == seen.end())
                seen.push_back(s);
            bool hit = (s.find(name) != std::string::npos) ||
                       (name.size() >= 2 && name.find(s) != std::string::npos && s.size() >= 2);
            if (!hit) continue;
            int x = (n.l + n.r) / 2, y = (n.t + n.b) / 2;
            if (x <= 0 || y <= 0) continue;
            TapXY(x, y);
            vTaskDelay(pdMS_TO_TICKS(900));
            return "已在手机上打开「" + s + "」\n" + ScreenBrief(1200);
        }
        SwipeDir("up");
        vTaskDelay(pdMS_TO_TICKS(550));
    }
    std::string list;
    for (const auto& s : seen) { if (list.size() > 400) break; list += s + "、"; }
    return "手机桌面上没找到「" + name + "」。真实可见的图标有：" + list;
}

std::string Apps() {
    GlobalAction("home");
    vTaskDelay(pdMS_TO_TICKS(650));
    std::vector<std::string> names;
    for (int page = 0; page < 3; ++page) {
        for (const auto& n : ReadNodes()) {
            const std::string& s = n.text;
            if (s.empty() || s.size() > 24) continue;
            if (std::find(names.begin(), names.end(), s) == names.end()) names.push_back(s);
        }
        SwipeDir("up");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    std::string out;
    for (const auto& s : names) { if (out.size() > 1800) break; out += s + "、"; }
    return out.empty() ? "（桌面没读到图标文字，请确认无障碍开关已打开）" : out;
}

// ---------- 中文整句 → 动作序列（全部在 ESP32 里） ----------
std::string CutWords(const std::string& s, const std::vector<const char*>& words) {
    size_t cut = s.size();
    for (const char* w : words) {
        size_t p = s.find(w);
        if (p != std::string::npos && p < cut) cut = p;
    }
    return Trim(s.substr(0, cut));
}

std::string ExtractApp(const std::string& t) {
    size_t p = t.find("打开");
    if (p == std::string::npos) return "";
    std::string s = t.substr(p + strlen("打开"));
    for (const char* pre : {"手机上的", "手机里的", "手机里", "手机上", "手机", "这个", "那个"}) {
        if (s.compare(0, strlen(pre), pre) == 0) { s = s.substr(strlen(pre)); break; }
    }
    return CutWords(s, {"，", ",", "。", "帮我", "给我", "然后", "接着", "再去", "再",
                        "并", "搜索", "搜一下", "查一下", "查", "找一下", "找", "回复",
                        "评论", "发消息", "发条消息", "发信息", "下单", "结算", "支付",
                        "付款", "播放", "听", "说", "点", "按", "输入", "读", "看", "给"});
}

std::string ExtractQuery(const std::string& t) {
    for (const char* k : {"搜索", "搜一下", "搜", "查一下", "查询", "查", "找一下", "找"}) {
        size_t p = t.find(k);
        if (p == std::string::npos) continue;
        std::string s = t.substr(p + strlen(k));
        return CutWords(s, {"，", ",", "。", "然后", "接着", "再", "按回车", "回车",
                            "多少钱", "什么价", "价格", "怎么样", "好不好", "播放", "听",
                            "帮我", "给我"});
    }
    return "";
}

std::string ExtractAfter(const std::string& t, const char* key) {
    size_t p = t.find(key);
    return p == std::string::npos ? "" : Trim(t.substr(p + strlen(key)));
}

bool HasAny(const std::string& t, const std::vector<const char*>& ws) {
    for (const char* w : ws) if (t.find(w) != std::string::npos) return true;
    return false;
}

std::string MakeToken() {
    char b[16];
    uint32_t v = esp_random();
    snprintf(b, sizeof(b), "%08x", (unsigned)(v & 0xffffffffu));
    return std::string(b, 8);
}

std::string RunSentence(const std::string& raw, bool confirmed) {
    std::string t = Trim(raw);
    if (t.empty()) return "没听清要做什么。";

    // 二次确认：用户说出「确认执行 <8位码>」才允许发送/下单这类动作。
    size_t cp = t.find("确认执行");
    if (cp != std::string::npos) {
        std::string given = Trim(t.substr(cp + strlen("确认执行")));
        std::string want, text;
        { Guard g; want = g_pending_token; text = g_pending_text; }
        if (want.empty() || given.compare(0, 8, want) != 0)
            return "没有待确认的动作，或确认码不对。请重新说一遍要做的事。";
        { Guard g; g_pending_token.clear(); g_pending_text.clear(); }
        return "已确认，开始执行。\n" + RunSentence(text, true);
    }

    bool sensitive = HasAny(t, {"发消息", "发条消息", "发信息", "回复", "评论",
                                "发送", "下单", "提交订单", "结算", "转账"});
    if (sensitive && !confirmed) {
        std::string tok = MakeToken();
        { Guard g; g_pending_token = tok; g_pending_text = t; }
        return "这是敏感操作，需要你再确认一次。确认请说「确认执行 " + tok +
               "」。\n当前手机屏幕：" + ScreenBrief(500);
    }

    std::string app = ExtractApp(t);
    std::string query = ExtractQuery(t);
    if (query.empty()) {
        for (const char* k : {"播放", "听一首", "听"}) {
            std::string s = ExtractAfter(t, k);
            if (!s.empty()) { query = CutWords(s, {"，", ",", "。", "的", "这首歌", "吧"}); break; }
        }
    }
    std::string message;
    for (const char* k : {"发消息说", "发信息说", "发消息", "回复", "评论", "说"}) {
        message = ExtractAfter(t, k);
        if (!message.empty()) break;
    }
    std::string contact;
    size_t gp = t.find("给");
    if (gp != std::string::npos) {
        size_t fp = t.find("发", gp);
        if (fp != std::string::npos && fp > gp) contact = Trim(t.substr(gp + 3, fp - gp - 3));
    }

    std::vector<std::string> steps;
    if (!app.empty()) steps.push_back(OpenApp(app));
    if (!query.empty()) {
        steps.push_back(ClickText("搜索"));
        steps.push_back(SetText(query));
        steps.push_back(PressEnter());
        vTaskDelay(pdMS_TO_TICKS(900));
    } else if (t.find("按回车") != std::string::npos || t.find("回车") != std::string::npos) {
        steps.push_back(PressEnter());
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    if (!contact.empty()) steps.push_back(ClickText(contact));
    if (!message.empty() && !query.empty()) steps.push_back(SetText(message));
    else if (!message.empty()) steps.push_back(SetText(message));
    if (sensitive && confirmed) steps.push_back(ClickText("发送"));
    if (t.find("上滑") != std::string::npos) steps.push_back(SwipeDir("up"));
    if (t.find("下滑") != std::string::npos) steps.push_back(SwipeDir("down"));
    if (t.find("返回") != std::string::npos) steps.push_back(GlobalAction("back"));

    std::string out = "【执行过程】\n";
    for (size_t i = 0; i < steps.size(); ++i)
        out += std::to_string(i + 1) + ") " + steps[i] + "\n";
    out += "\n【手机屏幕真实回读】\n" + ScreenBrief(1600);
    return out;
}

// ---------- base64（/keyboard/input 用） ----------
std::string B64Decode(const std::string& s) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int vals[256];
    for (int i = 0; i < 256; ++i) vals[i] = -1;
    for (int i = 0; i < 64; ++i) vals[(unsigned char)tbl[i]] = i;
    int acc = 0, bits = 0;
    std::string out;
    for (unsigned char c : s) {
        if (c == '=' || vals[c] < 0) continue;
        acc = (acc << 6) | vals[c];
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xff); }
    }
    return out;
}

// ---------- Portal 风格路由 → 手机动作 ----------
std::string Dispatch(const std::string& path, const std::string& body) {
    if (path == "/ping") return "pong (ESP32 直连手机桥, " + g_host + ")";
    if (path == "/version") return "xiaozhi-esp32 phone-bridge 1.0（零电脑 / 单 APK / 自动发现）";
    if (path == "/devices") return "{\"phone\":{\"online\":true,\"host\":\"" + g_host +
                                  "\"},\"mode\":\"esp32-direct\",\"computer\":false}";
    if (path == "/apps" || path == "/phone_apps") return "手机桌面上真实安装的应用：" + Apps();
    if (path == "/screen" || path == "/ui") return ScreenBrief();
    if (path == "/result" || path == "/state" || path == "/phone_state" ||
        path == "/browser_state" || path == "/music_state")
        return "手机当前状态：" + CurrentApp() + "\n" + ScreenBrief(1200);
    if (path == "/phone_bootstrap")
        return "已接入手机 " + g_host + "，不需要电脑、ADB 或 Termux。";
    if (path == "/app") {
        std::string pkg = JStr(body, "package");
        if (pkg.empty()) pkg = JStr(body, "name");
        return OpenApp(pkg);
    }
    if (path == "/tap" || path == "/click") {
        int x = JInt(body, "x"), y = JInt(body, "y");
        return TapXY(x, y) + "\n" + ScreenBrief(600);
    }
    if (path == "/swipe") {
        int x1 = JInt(body, "startX", JInt(body, "x1", -1));
        int y1 = JInt(body, "startY", JInt(body, "y1", -1));
        int x2 = JInt(body, "endX", JInt(body, "x2", -1));
        int y2 = JInt(body, "endY", JInt(body, "y2", -1));
        if (x1 < 0) {
            std::string d = JStr(body, "direction");
            return SwipeDir(d == "down" ? "down" : d == "left" ? "left" :
                            d == "right" ? "right" : "up") + "\n" + ScreenBrief(600);
        }
        char b[160];
        snprintf(b, sizeof(b), "{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}", x1, y1, x2, y2);
        std::string out;
        Eye("/swipe", std::string(b), out);
        return "已滑动\n" + ScreenBrief(600);
    }
    if (path == "/keyboard/input") {
        std::string txt = JStr(body, "base64_text");
        if (!txt.empty()) txt = B64Decode(txt);
        if (txt.empty()) txt = JStr(body, "text");
        return SetText(txt) + "\n" + ScreenBrief(500);
    }
    if (path == "/keyboard/key") {
        int k = JInt(body, "key_code", JInt(body, "key", -1));
        if (k == 4) return GlobalAction("back") + "\n" + ScreenBrief(600);
        if (k == 3) return GlobalAction("home") + "\n" + ScreenBrief(600);
        return PressEnter() + "\n" + ScreenBrief(600);
    }
    if (path == "/phone" || path == "/send_message" || path == "/play_music" ||
        path == "/browser" || path == "/parse_message") {
        std::string text = JStr(body, "text");
        if (text.empty()) text = JStr(body, "song");
        if (text.empty() && !body.empty() && body.front() != '{') text = body;
        return RunSentence(text, false);
    }
    return "暂不支持的动作 " + path + "。可以说「打开 X」「搜索 X」「读屏」「按回车」「上滑」。";
}

std::string Execute(const std::string& raw_path, const std::string& body) {
    // 固件侧 Portal 客户端可能带 ?brief=1 这类查询串，路由匹配前先剥掉。
    std::string path = raw_path;
    size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    bool active;
    { Guard g; active = g_active; }
    if (!active) {
        return "手机还没接入。请用手机浏览器打开 ESP32 的引导页（串口日志里有地址），"
               "下载并安装 19KB 的配套 App，再到 设置 → 无障碍 → 打开开关；"
               "ESP32 会自动发现手机并完成绑定，不用填 IP、不用电脑。";
    }
    // 本地 STT 兜底与云端 tools/call 会对同一句话各触发一次，按业务键去重。
    std::string key = path + "|" + body;
    uint32_t now = NowMs();
    {
        Guard g;
        if (key == g_last_key && now - g_last_key_ms < 15000 && !g_last_result.empty())
            return g_last_result;
    }
    std::string out = Dispatch(path, body);
    {
        Guard g;
        g_last_key = key; g_last_key_ms = now; g_last_result = out;
    }
    return out;
}

// ---------- 引导页 / APK 托管 / 自动发现 ----------
const char* kHtml = R"HTML(<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小智 · 手机直连接入</title><style>
body{font-family:-apple-system,system-ui,sans-serif;background:#f6f7f9;color:#1c1f23;
margin:0;padding:24px;max-width:640px;margin:auto;line-height:1.65}
h1{font-size:22px;margin:0 0 6px}.sub{color:#6b7280;font-size:14px;margin-bottom:20px}
.card{background:#fff;border:1px solid #e5e7eb;border-radius:14px;padding:18px;margin:14px 0}
.btn{display:block;text-align:center;background:#2563eb;color:#fff;text-decoration:none;
padding:14px;border-radius:10px;font-size:17px;font-weight:600;margin:12px 0}
.ok{color:#15803d;font-weight:600}.wait{color:#b45309;font-weight:600}
.step{padding:8px 0;border-bottom:1px solid #f0f1f3}.step:last-child{border:0}
b{color:#2563eb}code{background:#eef2ff;padding:2px 6px;border-radius:5px;font-size:13px}
small{color:#6b7280}
</style></head><body>
<h1>小智 · 手机直连接入</h1>
<div class="sub">ESP32 自己托管安装，不需要电脑 / ADB / root / Termux</div>
<div class="card"><div id="st">正在检查连接状态…</div>
<a class="btn" href="/xiaozhi-eye.apk" download="xiaozhi-eye.apk">① 下载配套 App（19 KB）</a>
<small>MIT 开源 orb-eye v2.0，一个无障碍服务，无联网上传、无账号。</small></div>
<div class="card">
<div class="step"><b>② 安装</b>：点上面按钮下载，按系统提示点「安装」。若提示「禁止安装未知应用」，在本浏览器/文件管理器的设置里允许一次即可。</div>
<div class="step"><b>③ 打开开关</b>：手机 设置 → 无障碍（或「辅助功能」）→ 已下载的服务 → <code>Orb Eye</code> → 打开。Android 13+ 若开关是灰的，先到 应用详情 → 右上角 ⋮ → 允许受限设置。</div>
<div class="step"><b>④ 完成</b>：回到这个页面，状态变成绿色「已连接」就可以直接对小智说话了。ESP32 会自动记住来访地址、必要时扫描同网段找到手机，不用填 IP、不用电脑、不用敲命令。</div>
</div>
<div class="card"><small><b>说明</b>：那根 C 口线只给 ESP32 供电；指令走 WiFi。装 App 和打开无障碍开关这两步 Android 强制由本人确认，任何设备都不能静默代做。该 App 的本地接口没有鉴权，请只在自己信任的 WiFi 下使用；支付、密码、验证码不自动执行，发消息/下单需语音二次确认。</small></div>
<script>
function refresh(){fetch('/status').then(r=>r.json()).then(d=>{
 var e=document.getElementById('st');
 e.innerHTML=d.active?'<span class="ok">✓ 已连接手机 '+d.phone+'</span> —— 现在可以直接对小智说「打开微信」「搜索奶茶多少钱」':
 '<span class="wait">○ 等待手机接入</span> —— 请先下载安装并打开无障碍开关';
}).catch(()=>{});}refresh();setInterval(refresh,4000);
</script></body></html>)HTML";

bool MatchAll(const char*, const char*, size_t) { return true; }

std::string StatusJson() {
    Guard g;
    std::string s = "{\"active\":";
    s += g_active ? "true" : "false";
    s += ",\"phone\":" + Jq(g_host);
    s += ",\"candidate\":" + Jq(g_candidate);
    s += ",\"apk_size\":" + std::to_string(kOrbEyeApkSize);
    s += ",\"mode\":\"esp32-direct\",\"computer\":false}";
    return s;
}

esp_err_t SendStr(httpd_req_t* req, const char* type, const std::string& s) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s.c_str(), (ssize_t)s.size());
}

void RecordVisitor(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        ESP_LOGW(TAG, "引导页: 拿不到请求 socket（改用局域网扫描发现手机）");
        return;
    }
    struct sockaddr_in peer = {};
    socklen_t len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr*)&peer, &len) != 0) {
        ESP_LOGW(TAG, "引导页: getpeername 失败 errno=%d", errno);
        return;
    }
    if (peer.sin_family != AF_INET) {
        ESP_LOGW(TAG, "引导页: 来访地址非 IPv4 (family=%d)", peer.sin_family);
        return;
    }
    char ip[32] = {0};
    if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip))) return;
    if (strncmp(ip, "127.", 4) == 0) return;   // 本机调试不算手机
    bool changed = false;
    {
        Guard g;
        changed = (g_candidate != ip);
        g_candidate = ip;
    }
    if (changed) ESP_LOGI(TAG, "引导页来访者 IP 已记住: %s（装好 App 会自动连上）", ip);
}

esp_err_t HandleAll(httpd_req_t* req) {
    RecordVisitor(req);
    std::string uri(req->uri);
    size_t q = uri.find('?');
    std::string path = q == std::string::npos ? uri : uri.substr(0, q);
    std::string query = q == std::string::npos ? "" : uri.substr(q + 1);

    std::string body;
    if (req->content_len > 0 && req->content_len < 8192) {
        char buf[1024];
        size_t total = 0;
        while (total < (size_t)req->content_len) {
            size_t want = std::min(sizeof(buf), (size_t)req->content_len - total);
            int n = httpd_req_recv(req, buf, want);
            if (n <= 0) break;
            body.append(buf, (size_t)n);
            total += (size_t)n;
        }
    }

    if (path == "/" || path == "/index.html" || path == "/setup")
        return SendStr(req, "text/html; charset=utf-8", kHtml);

    if (path == "/xiaozhi-eye.apk") {
        httpd_resp_set_type(req, "application/vnd.android.package-archive");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"xiaozhi-eye.apk\"");
        return httpd_resp_send(req, (const char*)kOrbEyeApk, (ssize_t)kOrbEyeApkSize);
    }
    if (path == "/status") return SendStr(req, "application/json; charset=utf-8", StatusJson());

    if (path == "/unbind") {
        {
            Guard g;
            g_candidate.clear(); g_saved_ip.clear(); g_host.clear(); g_active = false;
        }
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, kNvsKey); nvs_commit(h); nvs_close(h);
        }
        return SendStr(req, "application/json",
                       "{\"status\":\"success\",\"result\":\"已清除记住的手机，稍后自动重新扫描局域网\"}");
    }

    if (path == "/bind") {
        std::string ip;
        size_t p = query.find("ip=");
        if (p != std::string::npos) ip = query.substr(p + 3, 15);
        if (ip.empty()) return SendStr(req, "application/json", "{\"error\":\"用法 /bind?ip=192.168.1.5\"}");
        { Guard g; g_candidate = ip; g_saved_ip = ip; }
        SaveIp(ip);
        return SendStr(req, "application/json", "{\"status\":\"success\",\"result\":\"已记录手机 IP，正在探测 :7333\"}");
    }

    // 其余全部是固件 MCP 工具用的 Portal 兼容路由，返回同样的 {"status","result"} 包装。
    std::string result = Execute(path, body);
    std::string wrapped = "{\"status\":\"success\",\"result\":" + Jq(result) + "}";
    return SendStr(req, "application/json; charset=utf-8", wrapped);
}

// 原生 socket 轻量探测，避免占用 esp_http_client 的栈。
bool ProbeEye(const std::string& ip) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    // 慢速 WiFi 实测 RTT 可到 700ms+，超时给足 2.5s，避免误判掉线来回抖动。
    struct timeval tv = {2, 500000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(kEyePort);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1 || a.sin_addr.s_addr == 0) {
        close(s); return false;
    }
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return false; }
    const char* req = "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    if (send(s, req, strlen(req), 0) < 0) { close(s); return false; }
    char buf[512];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    close(s);
    if (n <= 0) return false;
    buf[n] = 0;
    return strstr(buf, "orb-eye") != nullptr;
}

// 已建立连接的 socket 上做一次 /ping，确认对方真的是手机的无障碍服务。
bool PingOnConnected(int s) {
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) fcntl(s, F_SETFL, fl & ~O_NONBLOCK);   // 恢复阻塞，便于收响应
    struct timeval tv = {2, 500000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const char* req = "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    if (send(s, req, strlen(req), 0) < 0) return false;
    char buf[512];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = 0;
    return strstr(buf, "orb-eye") != nullptr;
}

// 局域网兜底发现：不依赖引导页来访 IP，也不依赖 NVS 里的旧地址。
// 12 路非阻塞 connect 一批，一轮约 6~10 秒；换一台全新手机也能自动找到。
bool ScanSubnet(std::string& found) {
    esp_netif_t* ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (!ni || esp_netif_get_ip_info(ni, &info) != ESP_OK || info.ip.addr == 0) return false;
    const uint32_t self = info.ip.addr;
    const uint32_t mask = info.netmask.addr;
    const uint32_t net = self & mask;

    for (int base = 1; base < 255; base += 12) {
        int fds[12];
        uint32_t addrs[12];
        fd_set wf;
        FD_ZERO(&wf);
        int maxfd = -1;
        for (int k = 0; k < 12; ++k) { fds[k] = -1; addrs[k] = 0; }

        for (int k = 0; k < 12 && base + k < 255; ++k) {
            uint32_t addr = net | (htonl((uint32_t)(base + k)) & ~mask);
            if (addr == self || addr == net || addr == (net | ~mask)) continue;
            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s < 0) continue;
            int fl = fcntl(s, F_GETFL, 0);
            // 拿不到非阻塞能力就跳过，绝不留下可能挂死的阻塞 connect。
            if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) { close(s); continue; }
            struct sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_port = htons(kEyePort);
            a.sin_addr.s_addr = addr;
            if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0 && errno != EINPROGRESS) {
                close(s);
                continue;
            }
            fds[k] = s; addrs[k] = addr;
            FD_SET(s, &wf);
            if (s > maxfd) maxfd = s;
        }
        if (maxfd < 0) continue;

        struct timeval tv = {0, 350000};
        int ready = select(maxfd + 1, nullptr, &wf, nullptr, &tv);
        bool hit = false;
        for (int k = 0; k < 12; ++k) {
            if (fds[k] < 0) continue;
            if (ready > 0 && FD_ISSET(fds[k], &wf)) {
                int e = 0;
                socklen_t el = sizeof(e);
                if (getsockopt(fds[k], SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e == 0) {
                    char ip[32];
                    if (inet_ntop(AF_INET, &addrs[k], ip, sizeof(ip)) &&
                        PingOnConnected(fds[k])) {
                        found = ip;
                        hit = true;
                    }
                }
            }
            close(fds[k]);
        }
        if (hit) return true;
    }
    return false;
}

// 必须在 WiFi 拿到 IP、lwIP tcpip 任务起来之后才能调用；否则 httpd_start 里的
// socket() 会触发 `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)` 并反复重启。
void StartHttpd(const char* local_ip) {
    if (g_httpd_started) return;
    g_httpd_started = true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 12288;          // handler 里会同步跑 esp_http_client，栈要够
    cfg.max_uri_handlers = 2;
    cfg.max_open_sockets = 5;
    cfg.uri_match_fn = MatchAll;     // 单 handler 接所有路径，避免 query 串匹配问题
    if (httpd_start(&g_server, &cfg) == ESP_OK) {
        httpd_uri_t all = {};
        all.uri = "*";
        all.method = (httpd_method_t)HTTP_ANY;
        all.handler = HandleAll;
        httpd_register_uri_handler(g_server, &all);
        ESP_LOGI(TAG, "========== 手机接入引导页: http://%s/ ==========", local_ip);
        ESP_LOGI(TAG, "引导服务已启动：:80（APK %u bytes 内嵌在 flash）",
                 (unsigned)kOrbEyeApkSize);
        ESP_LOGI(TAG, "手机连同一 WiFi 打开这个地址 → 下载 19KB App → 打开无障碍开关，其余自动");
    } else {
        ESP_LOGE(TAG, "引导服务启动失败");
    }
}

void DiscoveryTask(void*) {
    bool announced = false;
    uint32_t last_log = 0;
    uint32_t last_scan = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2500));

        // 等到 TCP/IP 栈就绪才碰任何 socket：先打印引导页地址并起 httpd。
        if (!announced) {
            esp_netif_t* ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t info = {};
            if (ni && esp_netif_get_ip_info(ni, &info) == ESP_OK && info.ip.addr != 0) {
                char ip[32];
                inet_ntop(AF_INET, &info.ip.addr, ip, sizeof(ip));
                StartHttpd(ip);
                announced = true;
            }
            continue;   // 网络没起来之前不要探测手机
        }

        std::string cand;
        bool active;
        { Guard g; cand = g_candidate.empty() ? g_saved_ip : g_candidate; active = g_active; }
        uint32_t now = NowMs();

        // 全新手机 / 换过路由器：没有记住的地址时，自己扫一遍同网段。
        if (cand.empty() && !active && now - last_scan > 30000) {
            last_scan = now;
            ESP_LOGI(TAG, "没有记住的手机地址，开始扫描同网段 :7333 ...");
            std::string found;
            if (ScanSubnet(found)) {
                ESP_LOGI(TAG, "✓ 局域网扫描发现手机: %s", found.c_str());
                { Guard g; g_candidate = found; g_saved_ip = found; }
                SaveIp(found);
                cand = found;
            } else {
                ESP_LOGI(TAG, "本轮没扫到手机，30 秒后再试；也可用 phonebind <手机IP> 立即绑定");
                continue;
            }
        }
        if (cand.empty()) continue;

        bool ok = ProbeEye(cand);
        now = NowMs();   // 扫描可能耗时，刷新后再用于在线/掉线判定
        if (ok) {
            bool was = false;
            {
                Guard g;
                was = !g_active;
                g_host = cand + ":" + std::to_string(kEyePort);
                g_active = true;
                g_last_ok_ms = now;
                g_saved_ip = cand;
            }
            SaveIp(cand);
            if (was) ESP_LOGI(TAG, "✓ 自动发现手机 %s，已切换到 ESP32 直连模式（无需 macset）", g_host.c_str());
            else if (now - last_log > 60000) { last_log = now; ESP_LOGI(TAG, "手机在线 %s", g_host.c_str()); }
        } else if (active && now - g_last_ok_ms > 20000) {
            {
                Guard g;
                g_active = false;
                g_host.clear();
            }
            ESP_LOGW(TAG, "手机掉线，已回退到原 Portal：%s", "（macset 地址仍有效）");
        }
    }
}

}  // namespace

void StartPhoneBridge() {
    if (g_started) return;
    g_started = true;
    g_lock = xSemaphoreCreateMutex();
    LoadSavedIp();
    // httpd 与手机探测都等网络就绪后在 DiscoveryTask 里启动，
    // 这里只建任务，绝不能在板级构造函数阶段调用 socket()。
    xTaskCreate(DiscoveryTask, "phone_disc", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "手机直连桥已装备（等待 WiFi 就绪后启动引导服务）");
}

bool PhoneBridgeActive() {
    Guard g;
    return g_active;
}

std::string PhoneBridgeExecute(const std::string& path, const std::string& body) {
    return Execute(path, body);
}

std::string PhoneBridgeConsoleCommand(const std::string& cmd,
                                      const std::vector<std::string>& args) {
    if (cmd == "phonestatus") {
        return "手机直连桥：" + StatusJson();
    }
    if (cmd == "phonebind") {
        if (args.empty()) return "用法: phonebind <手机IP>";
        { Guard g; g_candidate = args[0]; g_saved_ip = args[0]; }
        SaveIp(args[0]);
        return "已记录手机 IP " + args[0] + "，正在探测 :7333";
    }
    if (cmd == "phoneunbind") {
        {
            Guard g;
            g_candidate.clear(); g_saved_ip.clear(); g_host.clear(); g_active = false;
        }
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, kNvsKey); nvs_commit(h); nvs_close(h);
        }
        return "已清除记住的手机 IP，等待自动重新扫描局域网（或 phonebind <IP>）";
    }
    if (cmd == "phonesim") {
        // 真正执行要放到 mac_http 的 8KB 任务里（串口任务只有 4KB），
        // 用既有的 macphone <整句> 即可，桥模式下会自动走固件直连。
        return "模拟手机整句请直接用: macphone <整句>（会自动走 ESP32 直连桥）";
    }
    return "";
}
