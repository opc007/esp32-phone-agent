#include "mac_control_tools.h"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "mcp_server.h"
#include "portal_client.h"
#include "phone_bridge.h"

#define TAG "MacCtrl"

namespace {

// 接入的设备 的局域网地址；IP 变了可在串口用 `macset <新IP:8080> [token]` 改，会存进 NVS。
constexpr const char* kDefaultHost = "10.197.1.38:8080";
constexpr const char* kDefaultToken = "test";

PortalClient& Portal() {
    return PortalClient::GetInstance();
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// 所有 HTTP 操作都在一个专用的 8KB 栈任务里执行。
// 原因：串口自测任务只有 4KB、云端 MCP 工具回调所在任务栈也不可控，
// 直接在里面跑 esp_http_client 会 stack overflow 并重启设备（已实测踩过）。
enum class Op { Ping, OpenApp, Status, Click, Input, ListApps, PlayMusic, SendMessage, Screen, Browser, Phone, Danmaku };

SemaphoreHandle_t g_lock = nullptr;
SemaphoreHandle_t g_start = nullptr;
SemaphoreHandle_t g_done = nullptr;
Op g_op = Op::Ping;
std::string g_arg;
std::string g_arg2;
int g_x = 0;
int g_y = 0;
std::string g_result;

// ESP32 直连手机模式：所有编排都在固件里，不再经过任何电脑/Termux。
// 必须用 C++ 直调而不是把 Portal 地址改成 127.0.0.1 —— 那会造成
// 「工作任务等 httpd 响应、httpd 又等工作任务」的自环死锁。
std::string BridgeExec(Op op, const std::string& arg, const std::string& arg2, int x, int y) {
    switch (op) {
        case Op::Ping:        return PhoneBridgeExecute("/ping", "");
        case Op::ListApps:    return PhoneBridgeExecute("/apps?brief=1", "");
        case Op::Status:      return PhoneBridgeExecute("/result", "");
        case Op::Screen:      return PhoneBridgeExecute("/screen", "");
        case Op::OpenApp:
            return PhoneBridgeExecute("/app",
                "{\"package\":\"" + JsonEscape(arg) + "\"}");
        case Op::PlayMusic:
            // arg 可能是云端工具给的整句 JSON，也可能是纯歌名。
            if (!arg.empty() && arg.front() == '{') return PhoneBridgeExecute("/play_music", arg);
            return PhoneBridgeExecute("/play_music", "{\"song\":\"" + JsonEscape(arg) + "\"}");
        case Op::SendMessage: return PhoneBridgeExecute("/send_message", arg);
        case Op::Browser:     return PhoneBridgeExecute("/browser", arg);
        case Op::Phone:       return PhoneBridgeExecute("/phone", arg);
        case Op::Click: {
            char b[80];
            snprintf(b, sizeof(b), "{\"x\":%d,\"y\":%d}", x * 1080 / 1000, y * 2400 / 1000);
            return PhoneBridgeExecute("/tap", b);
        }
        case Op::Input:
            return PhoneBridgeExecute("/keyboard/input",
                "{\"text\":\"" + JsonEscape(arg) + "\"}");
        case Op::Danmaku:
            // 抖音网页弹幕依赖电脑端 CDP 浏览器；手机直连模式下没有这条链路。
            return "当前是手机直连模式（没有电脑），抖音网页弹幕暂时只支持接入电脑后使用。";
    }
    return "未知操作";
}

std::string Exec(Op op, const std::string& arg, const std::string& arg2, int x, int y) {
    if (PhoneBridgeActive()) {
        return BridgeExec(op, arg, arg2, x, y);
    }
    std::string out;
    switch (op) {
        case Op::Ping:
            return Portal().Get("/ping", out, 4000)
                       ? std::string("OK (") + out + ")"
                       : std::string("失败: ") + Portal().LastError();
        case Op::OpenApp: {
            // 关键：固件里没有任何 App 名单，只把用户/模型说的名字原样发过去，
            // 由Portal 端从「真实安装列表」动态解析；找不到时错误里会带回可用应用列表。
            // arg2 非空 = 应用内搜索词：打开 App 后把词填进它自己的搜索框（耗时较长）。
            std::string body = "{\"package\":\"" + JsonEscape(arg) + "\"";
            if (!arg2.empty()) {
                body += ",\"search\":\"" + JsonEscape(arg2) + "\"";
            }
            body += "}";
            int timeout = arg2.empty() ? 10000 : 60000;
            if (Portal().Post("/app", body, out, timeout)) {
                return out.empty() ? std::string("已打开应用「") + arg + "」" : out;
            }
            return Portal().LastError();
        }
        case Op::ListApps: {
            if (!Portal().Get("/apps?brief=1", out, 12000)) {
                return "读取应用列表失败：" + Portal().LastError();
            }
            if (out.size() > 2600) out.resize(2600);
            return "接入的设备上真实安装的应用程序（实时读取，不是预设名单）：" + out;
        }
        case Op::PlayMusic: {
            // arg 可以是纯歌名，也可以是完整 JSON 请求体（含用户整句原文 text）。
            // 中文句式解析、曲库联想、真实播放、播放栏回读全在Portal 端做，
            // 固件不写死任何歌名、歌单、句式，也不假装自己有曲库。
            std::string body;
            if (!arg.empty() && arg.front() == '{') {
                body = arg;
            } else {
                body = "{\"song\":\"" + JsonEscape(arg) + "\"";
                if (!arg2.empty()) body += ",\"app\":\"" + JsonEscape(arg2) + "\"";
                body += "}";
            }
            if (Portal().Post("/play_music", body, out, 25000)) {
                return out.empty() ? std::string("已请求播放「") + arg + "」" : out;
            }
            return "播放失败：" + Portal().LastError();
        }
        case Op::Status:
            if (!Portal().Get("/result", out, 6000)) {
                return "读取设备状态失败：" + Portal().LastError();
            }
            if (out.size() > 900) out.resize(900);
            return "接入的设备当前状态（前台应用 / 最近动作 / 屏幕文字）：" + out;
        case Op::Screen: {
            // 真实控件树：不是写死的假界面，能看到微信是否登录、搜索框、聊天输入框等。
            if (!Portal().Get("/screen", out, 12000)) {
                return "读取设备屏幕失败：" + Portal().LastError();
            }
            if (out.size() > 2200) out.resize(2200);
            return "接入的设备屏幕上真实显示的内容：" + out;
        }
        case Op::Browser: {
            // arg 是完整 JSON 请求体：本地兜底只给整句原文 text，云端工具可给结构化字段。
            // Portal 端负责中文句式解析、真实打开浏览器、输入、按回车、读网页、点结果。
            if (Portal().Post("/browser", arg, out, 30000)) {
                return out.empty() ? std::string("已执行浏览器操作") : out;
            }
            return "浏览器操作失败：" + Portal().LastError();
        }
        case Op::Phone: {
            // arg 是完整 JSON 请求体。手机侧的应用名单、屏幕控件、坐标、中文句式
            // 全在Portal 端从真实接入的安卓设备读取/解析，固件零名单零句式。
            if (Portal().Post("/phone", arg, out, 30000)) {
                return out.empty() ? std::string("已执行手机操作") : out;
            }
            return "手机操作失败：" + Portal().LastError();
        }
        case Op::SendMessage: {
            // arg 已经是完整的 JSON 请求体（app/contact/message 或整句原文 text）。
            // Portal 端负责动态找应用、找联系人、判断登录状态，固件不存任何名单。
            if (Portal().Post("/send_message", arg, out, 30000)) {
                return out.empty() ? std::string("已执行发消息请求") : out;
            }
            // Portal 的 status:error 会带完整中文原因（含未登录 / 没找到联系人），
            // 必须原样交给大模型，它才能如实转述给用户，而不是瞎说「做不到」。
            return Portal().LastError();
        }
        case Op::Click: {
            // 大模型给的是 0-1000 相对坐标，Portal 用的是 1080x2400 坐标系。
            int px = x * 1080 / 1000;
            int py = y * 2400 / 1000;
            if (Portal().Tap(px, py)) {
                return "已在设备屏幕上点击相对坐标 (" + std::to_string(x) + "," +
                       std::to_string(y) + ")。";
            }
            return "点击失败：" + Portal().LastError();
        }
        case Op::Input:
            if (Portal().InputText(arg, true)) {
                return "已把文本输入到接入的设备（支持中文）。";
            }
            return "输入失败：" + Portal().LastError();
        case Op::Danmaku: {
            // arg 为空 = GET 读新增弹幕；非空 = POST 打开直播间（text/url 整句）。
            if (arg.empty()) {
                if (!Portal().Get("/danmaku", out, 20000)) {
                    return "读取弹幕失败：" + Portal().LastError();
                }
                if (out.size() > 4000) out.resize(4000);
                return out;
            }
            std::string body = (arg.front() == '{') ? arg
                : "{\"text\":\"" + JsonEscape(arg) + "\"}";
            if (Portal().Post("/danmaku", body, out, 35000)) {
                return out.empty() ? std::string("已打开抖音直播间") : out;
            }
            return "打开抖音直播间失败：" + Portal().LastError();
        }
    }
    return "未知操作";
}

void Worker(void*) {
    while (true) {
        if (xSemaphoreTake(g_start, portMAX_DELAY) == pdTRUE) {
            g_result = Exec(g_op, g_arg, g_arg2, g_x, g_y);
            xSemaphoreGive(g_done);
        }
    }
}

void InitWorker() {
    if (g_lock != nullptr) return;
    g_lock = xSemaphoreCreateMutex();
    g_start = xSemaphoreCreateBinary();
    g_done = xSemaphoreCreateBinary();
    xTaskCreate(Worker, "mac_http", 8192, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "Mac HTTP 专用任务已启动（8KB 栈）");
}

std::string Submit(Op op, const std::string& arg = "", const std::string& arg2 = "",
                   int x = 0, int y = 0, int wait_ms = 30000) {
    InitWorker();
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return "控制通道忙，请稍后再试";
    }
    g_op = op;
    g_arg = arg;
    g_arg2 = arg2;
    g_x = x;
    g_y = y;
    g_result.clear();
    xSemaphoreGive(g_start);
    bool ok = xSemaphoreTake(g_done, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
    std::string r = ok ? g_result : "控制设备超时（HTTP 未在限定时间内返回）";
    if (!ok) {
        // 清掉可能迟到的完成信号，避免影响下一次调用
        vTaskDelay(pdMS_TO_TICKS(200));
        while (xSemaphoreTake(g_done, 0) == pdTRUE) {}
    }
    xSemaphoreGive(g_lock);
    return r;
}

std::string OpenApp(const std::string& app, const std::string& search = "") {
    if (app.empty()) return "应用名不能为空";
    // 应用内搜索要经过 Tab 导航+注入+回读，电脑端最多 60 秒，等 70 秒留余量
    return Submit(Op::OpenApp, app, search, 0, 0, search.empty() ? 30000 : 70000);
}

std::string ListApps() {
    return Submit(Op::ListApps);
}

std::string PlayMusic(const std::string& song, const std::string& app = "") {
    if (song.empty()) return "歌曲名不能为空";
    return Submit(Op::PlayMusic, song, app);
}

// 发消息请求体：云端模型给结构化字段就直接用；本地语音兜底只给整句原文 text，
// 由Portal 端解析「打开微信给张三发消息说明天见」这类中文口语。
std::string MessageBody(const std::string& app, const std::string& contact,
                        const std::string& message, const std::string& text) {
    std::string body = "{";
    bool first = true;
    auto add = [&](const char* key, const std::string& v) {
        if (v.empty()) return;
        if (!first) body += ",";
        first = false;
        body += "\"";
        body += key;
        body += "\":\"";
        body += JsonEscape(v);
        body += "\"";
    };
    add("app", app);
    add("contact", contact);
    add("message", message);
    add("text", text);
    body += "}";
    return body;
}

std::string SendMessage(const std::string& app, const std::string& contact,
                        const std::string& message, const std::string& text = "") {
    return Submit(Op::SendMessage, MessageBody(app, contact, message, text));
}

std::string Screen() {
    return Submit(Op::Screen);
}

// 浏览器请求体：任何字段都可以空，Portal 端会按用户原话整句解析。
// 固件不写死句式、搜索引擎、网址，也不存任何网页内容。
std::string BrowserBody(const std::string& text, const std::string& action,
                        const std::string& query, const std::string& target,
                        const std::string& url, int index) {
    std::string body = "{";
    bool first = true;
    auto add = [&](const char* key, const std::string& v) {
        if (v.empty()) return;
        if (!first) body += ",";
        first = false;
        body += "\"" + std::string(key) + "\":\"" + JsonEscape(v) + "\"";
    };
    add("text", text);
    add("action", action);
    add("query", query);
    add("target", target);
    add("url", url);
    if (index > 0) {
        if (!first) body += ",";
        body += "\"index\":" + std::to_string(index);
    }
    body += "}";
    return body;
}

std::string Browser(const std::string& body) {
    return Submit(Op::Browser, body);
}

// 手机请求体：text 是用户原话整句，其余字段可空。
// 固件不解析中文、不存应用名、不存任何界面坐标；全部由Portal 端动态处理。
std::string PhoneBody(const std::string& text, const std::string& action,
                      const std::string& query, const std::string& confirm) {
    std::string body = "{";
    bool first = true;
    auto add = [&](const char* key, const std::string& v) {
        if (v.empty()) return;
        if (!first) body += ",";
        first = false;
        body += "\"" + std::string(key) + "\":\"" + JsonEscape(v) + "\"";
    };
    add("text", text);
    add("action", action);
    add("query", query);
    add("confirm", confirm);
    body += "}";
    return body;
}

std::string Phone(const std::string& body) {
    return Submit(Op::Phone, body);
}

std::string Status() {
    return Submit(Op::Status);
}

std::string Click(int x, int y) {
    return Submit(Op::Click, "", "", x, y);
}

std::string Input(const std::string& text) {
    return Submit(Op::Input, text);
}

std::string Join(const std::vector<std::string>& args) {
    std::string s;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) s += " ";
        s += args[i];
    }
    return s;
}

}  // namespace

void StartMacControlTools() {
    auto& portal = Portal();
    // 先给默认值，再尝试从 NVS 覆盖（用户改过 IP 时保留用户的设置）。
    portal.Set(kDefaultHost, kDefaultToken);
    portal.Load();
    ESP_LOGI(TAG, "Mac 控制目标: %s（应用名单由Portal 端动态提供，固件不存 App 表）",
             portal.Host().c_str());
    InitWorker();

    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.phone.control",
        "控制接入的安卓手机：打开 APP、读回屏幕真实文字和按钮、输入中文/按回车/"
        "点击按钮/上下滑动、查询装了什么应用、把外卖价格或评论等真实内容读回来。\n"
        "设备形态：Portal 可以直接跑在这台安卓手机本机（Termux 自托管，链路里没有电脑），"
        "也可以跑在一台 Mac 上再由手机接入。前一种情况下 self.mac.* 工具和本工具"
        "操作的是同一台手机，用户说「打开 X」「搜 X」「发消息」都要直接执行，"
        "不要因为没看到电脑就回答做不到。\n"
        "用户说「打开某应用」「查一下某东西多少钱」「搜索 X」「屏幕上有什么」"
        "「装了什么」「按回车」「点第一个结果」「上滑」「返回」「接入引导」，"
        "或提到要在 App 里做的事（外卖、短视频、聊天、回复、下单等）时，都必须调用本工具。\n"
        "极其重要：你实际控制着一台真实安卓手机，能读到它当前界面的控件树和文字，"
        "也能真的点击、输入、按回车。绝对不要回答「我无法控制手机」「我不能操作 APP」"
        "「请自己手动操作」「我没有手机权限」。\n"
        "用法：把用户原话整句填进 text，Portal 端会自动解析中文口语并拆成动作序列；"
        "也可用 action 指定单步动作（state/apps/open/input/enter/tap_text/back/home/swipe/ocr/bootstrap），"
        "配合 query 给出应用名或搜索词。\n"
        "找不到应用时，工具会带回这台设备真实装着的中文应用名，请按真实名字重说，不要编造应用。\n"
        "返回里带「手机屏幕真实内容」，回答用户时只依据这些内容，不要编造价格、结果或商品。"
        "发消息/回复/下单属于敏感动作，工具会返回一个确认码，请让用户再说一次"
        "「确认执行 <确认码>」并把码填进 confirm；支付、密码、验证码一律不由你执行。",
        PropertyList({
            Property("text", kPropertyTypeString, std::string("")).SetMaxLength(300),
            Property("action", kPropertyTypeString, std::string("")).SetMaxLength(30),
            Property("query", kPropertyTypeString, std::string("")).SetMaxLength(200),
            Property("confirm", kPropertyTypeString, std::string("")).SetMaxLength(30),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return Phone(PhoneBody(p["text"].value<std::string>(),
                                   p["action"].value<std::string>(),
                                   p["query"].value<std::string>(),
                                   p["confirm"].value<std::string>()));
        });

    mcp.AddTool(
        "self.mac.list_apps",
        "列出接入的设备上真实安装的全部应用程序。名单是 Portal 端实时从系统读取的，不是预设的。\n"
        "在打开任何应用之前必须先调用本工具，确认这台设备到底装了什么、准确名字叫什么。\n"
        "用户说「打开抖音」「打开QQ音乐」「这台设备能开什么」「你有哪些应用」时都要调用本工具。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return ListApps();
        });

    mcp.AddTool(
        "self.mac.open_app",
        "打开接入的设备上的应用程序。用户说「打开 X」「帮我开 X」「启动 X」时必须调用本工具，"
        "不要回答自己没有能力控制设备。\n"
        "重要：app_name 直接填用户说的名字，中文或英文都行，例如「抖音」「QQ音乐」「计算器」"
        "「Safari」「微信」；Portal 端会从真实安装列表里动态解析，固件里没有写死任何应用名。\n"
        "想在某应用里搜索内容时（如「打开抖音搜索三亚旅游」「在B站搜X」「打开QQ音乐搜周杰伦」），"
        "也调用本工具：app_name 填应用名，search 填搜索词，电脑会打开该应用并把词填进它自己的搜索框。"
        "绝不要因为用户说了「搜索」就改用浏览器工具——只有用户明确说「浏览器/网页」才用浏览器。\n"
        "拿不准就先调用 self.mac.list_apps。如果这个名字不存在，本工具会返回真实可用的应用列表，"
        "请从列表里选一个正确的名字重新调用，不要直接说打不开。",
        PropertyList({
            Property("app_name", kPropertyTypeString),
            Property("search", kPropertyTypeString, std::string("")).SetMaxLength(120),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return OpenApp(p["app_name"].value<std::string>(),
                           p["search"].value<std::string>());
        });

    mcp.AddTool(
        "self.mac.play_music",
        "在接入的设备上按名字搜索并真实播放指定歌曲。用户说「播放 X」「放一首 X」"
        "「打开 QQ音乐播放 X」「用 QQ音乐搜索 X」「搜索 X 这首歌来播放」「找一首 X」"
        "「我想听 X」「点播 X」「用 QQ音乐听 X」时，都必须调用本工具。\n"
        "极其重要：你控制的是一台真实设备，Portal 端会从它真实装着的音乐应用里"
        "动态找到并搜索播放，或从官方曲库取试听音频交给客户端，并读回界面确认当前歌曲。"
        "你本身不需要有曲库。绝对不要回答「没有这个曲库」「我无法播放音乐」"
        "「找不到这首歌」「没有这个内容」「我只能打开应用」。\n"
        "song 直接填歌曲名原文（中文即可，识别有个把错字也没关系，Portal 端会做曲库联想和相似度校正）。"
        "工具返回里如果说明是 VIP/未确认播放，请如实转述，不要断言正在播放。",
        PropertyList({
            Property("song", kPropertyTypeString).SetMaxLength(100),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return PlayMusic(p["song"].value<std::string>());
        });

    mcp.AddTool(
        "self.mac.send_message",
        "在接入的设备上打开微信/QQ等聊天软件，找到指定联系人，并发送指定内容的消息。"
        "这是一条完整的复合动作：打开应用 + 搜索联系人 + 输入消息 + 点击发送，一次调用全部完成。\n"
        "用户说「打开微信给张三发消息说明天见」「用微信给文件传输助手发条消息：测试一下」"
        "「给老婆发微信说今晚加班」「用QQ给小明发消息说在吗」「帮我给某人发个消息」时，"
        "必须调用本工具。\n"
        "极其重要：你实际控制着一台装有微信、QQ 的真实设备，可以读写它的界面控件，"
        "绝对不要回答「我无法给指定用户发消息」「我不能操作微信」「请自己手动发送」。\n"
        "contact 填联系人/群名原文，message 填要发送的内容原文；app 不填默认微信，"
        "用户提到 QQ/企业微信/钉钉就填那个名字（Portal 端会从真实安装列表动态解析）。\n"
        "拿不准句子里谁是联系人、哪句是内容时，把用户原话整句填进 text，Portal 端会自动解析。\n"
        "如果工具返回「需要登录」「没找到这个联系人」「会话未确认」，请如实把原因和下一步转述给用户，"
        "不要编造已发送，也不要改口说自己没有能力。",
        PropertyList({
            Property("contact", kPropertyTypeString).SetMaxLength(120),
            Property("message", kPropertyTypeString).SetMaxLength(300),
            Property("app", kPropertyTypeString, std::string("微信")).SetMaxLength(60),
            Property("text", kPropertyTypeString, std::string("")).SetMaxLength(300),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return SendMessage(p["app"].value<std::string>(),
                               p["contact"].value<std::string>(),
                               p["message"].value<std::string>(),
                               p["text"].value<std::string>());
        });

    mcp.AddTool(
        "self.mac.browser",
        "控制接入的设备上的浏览器：打开浏览器、在搜索框里输入中文、真实按下回车搜索、"
        "读回网页上真实显示的文字、点击搜索结果或网页按钮（如播放、登录、下一页）。\n"
        "只用浏览器的场景：用户明确说「浏览器」「网页」「打开某个网址」，或要查公开网上资料"
        "且没有指定任何应用（如「查一下今天天气」）。\n"
        "禁止场景：用户想在某个应用里看或搜内容时（如「打开抖音搜索X」「在B站搜X」"
        "「微信里搜小程序」），绝不能调用本工具，改用 self.mac.open_app 并填 search 参数。\n"
        "用户说「打开浏览器然后搜索 X」「帮我搜一下 X」「按回车」「点第一个结果」「点播放」"
        "「读一下网页上写了什么」「返回上一页」时调用本工具。\n"
        "极其重要：你实际控制着一台真实设备上的浏览器，能输入、能按回车、能读到网页内容。"
        "绝对不要回答「我无法操作浏览器」「我没有联网」「请自己手动搜索」「我做不到」。\n"
        "用法：把用户的原话整句填进 text，Portal 端会自动解析中文口语句式；"
        "也可以额外用 action/query/target/url 指定动作（action 可为 open/search/enter/read/click/back/type）。\n"
        "工具返回值里带有「网页上真实显示的内容」和主要链接，回答用户时只能依据这些内容，"
        "不要编造搜索结果、价格或网页文字。",
        PropertyList({
            Property("text", kPropertyTypeString, std::string("")).SetMaxLength(300),
            Property("action", kPropertyTypeString, std::string("")).SetMaxLength(30),
            Property("query", kPropertyTypeString, std::string("")).SetMaxLength(200),
            Property("target", kPropertyTypeString, std::string("")).SetMaxLength(60),
            Property("url", kPropertyTypeString, std::string("")).SetMaxLength(300),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return Browser(BrowserBody(p["text"].value<std::string>(),
                                       p["action"].value<std::string>(),
                                       p["query"].value<std::string>(),
                                       p["target"].value<std::string>(),
                                       p["url"].value<std::string>(), 0));
        });

    mcp.AddTool(
        "self.mac.screen",
        "读取接入的设备屏幕上真实显示的内容：前台应用、窗口标题、按钮文字、输入框、聊天文字、"
        "焦点位置和坐标。这是从系统辅助功能控件树实时读取的，不是预设的假界面。\n"
        "用户问「微信现在什么状态」「屏幕上有什么」「打开了吗」「发成功了吗」"
        "「这个应用登录了没有」时调用本工具；执行发消息前也可以先调用它确认界面状态。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return Screen();
        });

    mcp.AddTool(
        "self.mac.status",
        "查看接入的设备的当前状态：前台应用、最近执行过的操作、当前屏幕文字。"
        "用户问「现在开着什么」「刚才打开了吗」「做到哪一步了」时调用本工具。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return Status();
        });

    mcp.AddTool(
        "self.mac.click",
        "在接入的设备屏幕上点击指定位置。x、y 是 0-1000 的相对坐标："
        "左上角(0,0)，正中间(500,500)，右下角(1000,1000)。",
        PropertyList({
            Property("x", kPropertyTypeInteger, 0, 1000),
            Property("y", kPropertyTypeInteger, 0, 1000),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return Click(p["x"].value<int>(), p["y"].value<int>());
        });

    mcp.AddTool(
        "self.mac.input_text",
        "在接入的设备上输入文本（支持中文）。输入前建议先用 self.mac.click 点一下输入框让它获得焦点。",
        PropertyList({
            Property("text", kPropertyTypeString).SetMaxLength(200),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return Input(p["text"].value<std::string>());
        });

    mcp.AddTool(
        "self.danmaku.web",
        "读取抖音直播间里观众发的新弹幕（通过电脑浏览器实时读网页，不需要摄像头）。\n"
        "用户说「打开抖音直播间」时：调用本工具，把用户原话填进 text 打开直播间；"
        "也可在 text 里直接给直播间链接或房间号。\n"
        "用户说「读读弹幕」「有新弹幕就告诉我」「帮我盯着直播间」「播报弹幕」时："
        "不带参数调用，返回上次以来新出现的弹幕。\n"
        "要持续监听播报时：反复无参调用，两次间隔约10秒，直到用户叫停；"
        "返回 new 为空数组表示这段时间没有新弹幕，继续调用即可。\n"
        "返回的是观众真实发的弹幕文字，回答时如实转述，不要编造弹幕。",
        PropertyList({
            Property("text", kPropertyTypeString, std::string("")).SetMaxLength(300),
        }),
        [](const PropertyList& p) -> ReturnValue {
            return DanmakuWeb(p["text"].value<std::string>());
        });

    ESP_LOGI(TAG, "已注册 11 个设备端工具: phone.control / list_apps / open_app / play_music / browser / send_message / screen / status / click / input_text / danmaku.web");
}

std::string DanmakuWeb(const std::string& arg) {
    return Submit(Op::Danmaku, arg);
}

std::string MacControlConsoleCommand(const std::string& cmd,
                                     const std::vector<std::string>& args) {
    if (cmd == "macdanmakuweb") {
        // 无参=读新增弹幕；带参=整句原文打开直播间（和语音走同一条链路）
        if (args.empty()) return DanmakuWeb("");
        return DanmakuWeb(std::string("{\"text\":\"") + JsonEscape(Join(args)) + "\"}");
    }
    if (cmd == "say") {
        // 模拟云端返回一条 STT 文本，走与真实语音完全相同的本地指令识别路径。
        std::string text = Join(args);
        if (text.empty()) return "用法: say <模拟用户说的一句话>，例如 say 打开抖音";
        bool hit = MacVoiceCommandHook(text.c_str());
        return std::string("模拟 STT「") + text + "」→ " +
               (hit ? "命中本地指令（动作异步执行，看 [LOCAL-CMD] 日志）"
                    : "未命中本地指令，交给云端处理");
    }
    if (cmd == "mactest") {
        auto& mcp = McpServer::GetInstance();
        mcp.ParseMessage(R"({"jsonrpc":"2.0","id":901,"method":"tools/list","params":{}})",
            [](const std::string& r) {
                bool has = r.find("self.mac.list_apps") != std::string::npos;
                ESP_LOGI(TAG, "[MCP-SIM] tools/list 响应 %u 字节，含 self.mac.list_apps=%s",
                         (unsigned)r.size(), has ? "是" : "否");
            });
        mcp.ParseMessage(
            R"({"jsonrpc":"2.0","id":902,"method":"tools/call","params":{"name":"self.mac.open_app","arguments":{"app_name":"抖音"}}})",
            [](const std::string& r) {
                ESP_LOGI(TAG, "[MCP-SIM] tools/call 响应: %s", r.c_str());
            });
        return "已模拟云端 tools/list + tools/call（会动态打开「抖音」），看上面 [MCP-SIM] 响应";
    }
    if (cmd == "mac") {
        return "控制目标=" + Portal().Host() + " 连通=" + Submit(Op::Ping);
    }
    if (cmd == "macapps") {
        return ListApps();
    }
    if (cmd == "macapp") {
        if (args.empty()) return "用法: macapp <应用名> [搜索词]，例如 macapp 抖音 或 macapp 抖音 三亚旅游攻略";
        std::string q;
        for (size_t i = 1; i < args.size(); i++) {
            if (i > 1) q += " ";
            q += args[i];
        }
        return OpenApp(args[0], q);
    }
    if (cmd == "macplay") {
        if (args.empty()) return "用法: macplay <歌曲名>，例如 macplay 世上只有妈妈好";
        return PlayMusic(Join(args));
    }
    if (cmd == "macsong") {
        // 模拟一句真实语音：整句原文交给Portal 端解析（搜索/点歌/指定应用）
        if (args.empty()) return "用法: macsong <一句中文，如 打开QQ音乐搜索世上只有妈妈好来播放>";
        return Submit(Op::PlayMusic, std::string("{\"text\":\"") + JsonEscape(Join(args)) + "\"}");
    }
    if (cmd == "macbrowser") {
        // 模拟一句真实语音：打开浏览器 / 搜索 / 按回车 / 点结果 / 读网页
        if (args.empty()) return "用法: macbrowser <一句中文，如 打开浏览器搜索世上只有妈妈好>";
        return Browser(std::string("{\"text\":\"") + JsonEscape(Join(args)) + "\"}");
    }
    if (cmd == "macphone") {
        // 模拟一句真实语音：整句交给Portal 端，由手机执行器动态读屏/找应用/执行动作
        if (args.empty()) return "用法: macphone <一句中文，如 打开手机上的美团找一下奶茶多少钱>";
        return Phone(std::string("{\"text\":\"") + JsonEscape(Join(args)) + "\"}");
    }
    if (cmd == "phone") {
        // 结构化调试：phone <action> [query]，例如 phone apps / phone open 微信
        if (args.empty()) return "用法: phone <action> [query]，action 见 self.phone.control";
        std::string q = args.size() > 1 ? args[1] : "";
        return Phone(PhoneBody("", args[0], q, ""));
    }
    if (cmd == "macmsg") {
        // 模拟一句真实语音文本，走Portal 端口语解析 + 发消息完整链路
        if (args.empty()) return "用法: macmsg <一句中文，如 打开微信给张三发消息说明天见>";
        return SendMessage("", "", "", Join(args));
    }
    if (cmd == "macsend") {
        if (args.size() < 3) return "用法: macsend <应用> <联系人> <消息内容>，例如 macsend 微信 文件传输助手 你好";
        std::vector<std::string> msg(args.begin() + 2, args.end());
        return SendMessage(args[0], args[1], Join(msg));
    }
    if (cmd == "macscreen") {
        return Screen();
    }
    if (cmd == "macstatus") {
        return Status();
    }
    if (cmd == "macclick") {
        if (args.size() < 2) return "用法: macclick <x 0-1000> <y 0-1000>";
        return Click(std::atoi(args[0].c_str()), std::atoi(args[1].c_str()));
    }
    if (cmd == "macinput") {
        if (args.empty()) return "用法: macinput <要输入的文字（可含中文）>";
        return Input(Join(args));
    }
    if (cmd == "macset") {
        if (args.empty()) return "用法: macset <IP:8080> [token]";
        std::string token = args.size() > 1 ? args[1] : kDefaultToken;
        bool saved = Portal().Save(args[0], token);
        return std::string("已设置目标 ") + Portal().Host() +
               (saved ? "（已写入 NVS，重启保留）" : "（NVS 写入失败，仅本次生效）");
    }
    return "";
}

// ---------------------------------------------------------------------------
// 本地语音指令兜底
//
// 云端大模型可能因角色设定回答「我做不到」。这里不依赖它：只要小智听清了这句话，
// 设备端就解析意图并执行。重点是固件里没有任何 App 名单——所有名字都原样交给
// Portal 端，由Portal 端从真实安装列表动态解析。
// ---------------------------------------------------------------------------
namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// 去掉句尾语气词、标点和赘词
std::string TrimTail(std::string s) {
    static const char* kTail[] = {
        "，", "。", "！", "？", "、", "；", "：", "吧", "呢", "啊", "了", "呗", "嘛",
        "一下", "好吗", "可以吗", "谢谢", "应用", "软件", "程序", "帮我", " ",
        "的给去", "给去", "的歌", "这首歌", "给我",
    };
    bool changed = true;
    while (changed && !s.empty()) {
        changed = false;
        for (auto p : kTail) {
            size_t n = strlen(p);
            if (s.size() >= n && s.compare(s.size() - n, n, p) == 0) {
                s.resize(s.size() - n);
                changed = true;
                break;
            }
        }
        if (!changed && !s.empty()) {
            unsigned char c = (unsigned char)s.back();
            if (c < 0x80 && (ispunct(c) || isspace(c))) {
                s.pop_back();
                changed = true;
            }
        }
    }
    return s;
}

// 去掉开头多余空格/标点（ASR 偶尔会带一个前导空格）
std::string TrimHead(std::string s) {
    static const char* kCnPunct[] = {"，", "。", "！", "？", "、", "；", "："};
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (isspace(c) || ispunct(c)) { i++; continue; }
            break;
        }
        bool cn_punct = false;
        if (i + 2 < s.size()) {
            std::string seq = s.substr(i, 3);
            for (auto p : kCnPunct) {
                if (seq == p) { cn_punct = true; break; }
            }
        }
        if (cn_punct) { i += 3; continue; }
        break;
    }
    return s.substr(i);
}

struct VoiceJob {
    Op op;
    std::string arg;
    std::string arg2;
    int x = 0;
    int y = 0;
    std::string desc;
};

void VoiceWorker(void* p) {
    auto* job = static_cast<VoiceJob*>(p);
    // Submit 内部由 mac_http 专用任务（8KB 栈）执行 HTTP，这里只等结果，不会爆栈。
    std::string r = Submit(job->op, job->arg, job->arg2, job->x, job->y);
    ESP_LOGI(TAG, "[LOCAL-CMD] %s => %s", job->desc.c_str(), r.c_str());
    delete job;
    vTaskDelete(nullptr);
}

std::string g_last_voice;
int64_t g_last_voice_us = 0;

}  // namespace

extern "C" bool MacVoiceCommandHook(const char* text) {
    if (text == nullptr || *text == '\0') return false;
    std::string s(text);
    if (s.size() > 200) s.resize(200);

    // 同一句话 3 秒内只执行一次（云端偶尔会重复推送 STT）
    int64_t now = esp_timer_get_time();
    if (s == g_last_voice && (now - g_last_voice_us) < 3000000) return true;
    g_last_voice = s;
    g_last_voice_us = now;

    Op op = Op::Ping;
    std::string arg, arg2, desc;
    int x = 0, y = 0;
    bool hit = false;

    // 0) 手机指令优先：句子里明确提到手机/安卓时，整句交给手机通道。
    //    必须排在输入/点击/浏览器/点歌/打开应用之前，否则「打开手机上的美团找奶茶
    //    多少钱」会被当成在设备上打开一个名字很长的 App。固件只判断目标设备，
    //    不解析句式、不存任何手机应用名。
    if (!hit) {
        static const char* kPhoneWords[] = {"手机", "安卓", "Android", "android", "移动端"};
        for (auto w : kPhoneWords) {
            if (s.find(w) != std::string::npos) {
                op = Op::Phone;
                arg = std::string("{\"text\":\"") + JsonEscape(s) + "\"}";
                desc = "手机（原文「" + s + "」）";
                hit = true;
                break;
            }
        }
    }

    // 1) 输入文字
    static const char* kInputWords[] = {"输入", "打字", "写入"};
    for (auto w : kInputWords) {
        size_t p = s.find(w);
        if (p != std::string::npos) {
            std::string t = TrimHead(TrimTail(s.substr(p + strlen(w))));
            if (!t.empty()) {
                op = Op::Input;
                arg = t;
                desc = "在设备上输入「" + t + "」";
                hit = true;
            }
            break;
        }
    }

    // 2) 点击坐标
    if (!hit) {
        size_t p = s.find("点击");
        if (p != std::string::npos && sscanf(s.c_str() + p + strlen("点击"), "%d %d", &x, &y) == 2) {
            op = Op::Click;
            desc = "点击设备屏幕 (" + std::to_string(x) + "," + std::to_string(y) + ")";
            hit = true;
        }
    }

    // 2.7) 浏览器：打开浏览器 / 搜索 / 按回车 / 点结果 / 读网页。
    //      必须排在「点歌」和「打开应用」之前，否则「打开浏览器搜索 X」
    //      会被当成打开一个叫「浏览器搜索 X」的 App，或者被点歌逻辑截走。
    //      固件只判断意图，整句原文交给Portal 端解析，不写死任何句式。
    if (!hit) {
        static const char* kBrowserWords[] = {
            "浏览器", "网页", "上网", "百度一下", "必应", "谷歌一下",
            "按回车", "敲回车", "回车", "点第", "搜索结果", "读一下",
            "看看结果", "返回上一页", "上一页",
        };
        bool browserish = false;
        for (auto w : kBrowserWords) {
            if (s.find(w) != std::string::npos) { browserish = true; break; }
        }
        if (!browserish) {
            // 纯「搜索 X / 查一下 X」默认是网页搜索；但带音乐词的交给点歌工具
            bool musicish = s.find("音乐") != std::string::npos ||
                            s.find("歌") != std::string::npos ||
                            s.find("曲") != std::string::npos ||
                            s.find("听") != std::string::npos ||
                            s.find("播放") != std::string::npos;
            static const char* kSearchWords[] = {
                "搜索", "搜一下", "搜搜", "搜", "查一下", "查查", "查找",
                "找一下", "找找", "百度一下",
            };
            if (!musicish) {
                for (auto w : kSearchWords) {
                    if (s.find(w) != std::string::npos) { browserish = true; break; }
                }
            }
        }
        if (browserish) {
            op = Op::Browser;
            arg = std::string("{\"text\":\"") + JsonEscape(s) + "\"}";
            desc = "浏览器（原文「" + s + "」）";
            hit = true;
        }
    }

    // 3) 点歌：固件只判断「这是点歌意图」，把用户整句原文交给Portal 端解析。
    //    Portal 端负责中文句式、曲库联想、取音频、交给真实音乐客户端播放并回读状态。
    //    必须排在「打开应用」之前，否则「打开QQ音乐搜索晴头来播放」会被当成打开一个长名字的 App。
    if (!hit) {
        static const char* kStrong[] = {
            "播放", "放一首", "放首", "来一首", "来首", "点一首", "点播",
            "点歌", "放歌", "我想听", "听一下", "听首",
        };
        static const char* kWeak[] = {"搜索", "搜一下", "搜首", "找一首", "找首", "搜"};
        bool want = false;
        for (auto w : kStrong) {
            if (s.find(w) != std::string::npos) { want = true; break; }
        }
        if (!want) {
            bool musicish = s.find("歌") != std::string::npos ||
                            s.find("音乐") != std::string::npos ||
                            s.find("曲") != std::string::npos ||
                            s.find("听") != std::string::npos;
            if (musicish) {
                for (auto w : kWeak) {
                    if (s.find(w) != std::string::npos) { want = true; break; }
                }
            }
        }
        if (want) {
            op = Op::PlayMusic;
            arg = std::string("{\"text\":\"") + JsonEscape(s) + "\"}";
            desc = "点歌";
            hit = true;
        }
    }

    // 3.5) 复合指令：打开微信/QQ 给指定联系人发指定消息。
    //      必须排在「打开应用」之前，否则「打开微信给张三发消息…」会被当成
    //      打开一个名字很长的 App。固件不解析中文句式，只把原话整句交给Portal 端。
    if (!hit) {
        static const char* kMsgWords[] = {
            "发消息", "发条消息", "发一条消息", "发个消息", "发信息", "发条信息",
            "发微信", "发条微信", "发个微信", "发短信", "发条短信", "发个信息",
        };
        bool msg_word = false;
        for (auto w : kMsgWords) {
            if (s.find(w) != std::string::npos) { msg_word = true; break; }
        }
        bool recipient = s.find("给") != std::string::npos ||
                         s.find("跟") != std::string::npos ||
                         s.find("向") != std::string::npos ||
                         s.find("和") != std::string::npos;
        if (msg_word && recipient) {
            op = Op::SendMessage;
            arg = std::string("{\"text\":\"") + JsonEscape(s) + "\"}";
            desc = "发消息（原文「" + s + "」）";
            hit = true;
        }
    }

    // 3.9) 没说「手机」但「打开 X 之后还要查/找/回复/下单」的复合句：
    //      设备上打开应用不会带后续动作，这类基本都是手机 App 场景，整句交给手机通道。
    //      浏览器、点歌、发消息各自已经在上面命中，这里不会被误抢。
    if (!hit) {
        bool openish = s.find("打开") != std::string::npos ||
                       s.find("启动") != std::string::npos ||
                       s.find("进一下") != std::string::npos;
        bool follow = false;
        static const char* kFollow[] = {
            "多少钱", "价格", "贵不贵", "找一下", "找找", "帮我找", "查一下", "查查",
            "搜一下", "看一下", "看下", "有没有", "点第一", "回复", "评论", "下单",
            "推荐", "最便宜", "近的",
        };
        for (auto w : kFollow) {
            if (s.find(w) != std::string::npos) { follow = true; break; }
        }
        if (openish && follow) {
            op = Op::Phone;
            arg = std::string("{\"text\":\"") + JsonEscape(s) + "\"}";
            desc = "手机复合指令（原文「" + s + "」）";
            hit = true;
        }
    }

    // 4) 打开应用：不查任何本地别名表，名字原样送给Portal 端动态解析
    if (!hit) {
        size_t pos = std::string::npos;
        static const char* kZhTriggers[] = {
            "帮我打开", "帮我开", "打开一下", "打开", "开启", "启动", "运行", "开一下",
        };
        for (auto t : kZhTriggers) {
            size_t p = s.find(t);
            if (p != std::string::npos) {
                size_t end = p + strlen(t);
                if (pos == std::string::npos || end > pos) pos = end;
            }
        }
        std::string low = Lower(s);
        static const char* kEnTriggers[] = {"open ", "launch ", "start "};
        for (auto t : kEnTriggers) {
            size_t p = low.find(t);
            if (p != std::string::npos) {
                size_t end = p + strlen(t);
                if (pos == std::string::npos || end > pos) pos = end;
            }
        }
        if (pos != std::string::npos) {
            std::string name = TrimHead(TrimTail(s.substr(pos)));
            if (!name.empty() && name.size() <= 60) {
                op = Op::OpenApp;
                arg = name;
                desc = "打开设备应用「" + name + "」";
                hit = true;
            }
        }
    }

    // 5) 查询设备状态 / 应用列表
    if (!hit && (s.find("设备状态") != std::string::npos ||
                 s.find("设备现在") != std::string::npos ||
                 s.find("查看设备") != std::string::npos)) {
        op = Op::Status;
        desc = "查看设备状态";
        hit = true;
    }
    if (!hit && (s.find("有什么应用") != std::string::npos ||
                 s.find("装了什么") != std::string::npos ||
                 s.find("应用列表") != std::string::npos)) {
        op = Op::ListApps;
        desc = "读取设备真实安装的应用列表";
        hit = true;
    }

    if (!hit) return false;

    auto* job = new VoiceJob();
    job->op = op;
    job->arg = arg;
    job->arg2 = arg2;
    job->x = x;
    job->y = y;
    job->desc = desc;
    // 协议回调任务里绝不能阻塞等待 HTTP，动作丢到独立任务执行
    xTaskCreate(VoiceWorker, "mac_voice", 8192, job, 3, nullptr);
    ESP_LOGI(TAG, "[LOCAL-CMD] 命中语音指令：%s（原文「%s」）", desc.c_str(), s.c_str());
    return true;
}
