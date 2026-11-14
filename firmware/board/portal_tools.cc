/*
 * portal_tools.cc —— 把 Portal（安卓无障碍通道）注册成小智的 MCP 工具
 *
 * 策略（混合风控）：
 *   · 读屏：走 Portal /state（只在非敏感界面用）
 *   · 动作：优先走 HID（不可检测），HID 不可用时退回 Portal
 */
#include "portal_tools.h"

#include <esp_log.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <cJSON.h>

#include "mcp_server.h"
#include "phone_ble_hid.h"
#include "phone_hid.h"
#include "portal_client.h"

#define TAG "PortalTools"

namespace {

// 供 tap/swipe 做归一化坐标→像素换算；读屏后会被更新为真实分辨率
int s_screen_w = 1080;
int s_screen_h = 2400;

struct UiNode {
    std::string label;
    std::string cls;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool has_bounds = false;
    int cx() const { return (x1 + x2) / 2; }
    int cy() const { return (y1 + y2) / 2; }
};

constexpr int kMaxNodes = 80;
constexpr int kMaxDepth = 14;

bool ParseBounds(const char* raw, UiNode& n) {
    if (!raw) return false;
    int a, b, c, d;
    if (sscanf(raw, "%d,%d,%d,%d", &a, &b, &c, &d) != 4) return false;
    n.x1 = a; n.y1 = b; n.x2 = c; n.y2 = d;
    n.has_bounds = true;
    return true;
}

std::string LastSegment(const char* s, char sep) {
    if (!s) return "";
    std::string v(s);
    size_t p = v.rfind(sep);
    return p == std::string::npos ? v : v.substr(p + 1);
}

void Walk(cJSON* list, int depth, std::vector<UiNode>& nodes, std::string& text) {
    if (!cJSON_IsArray(list) || depth > kMaxDepth) return;
    cJSON* node = nullptr;
    cJSON_ArrayForEach(node, list) {
        if ((int)nodes.size() >= kMaxNodes) return;
        if (!cJSON_IsObject(node)) continue;

        UiNode n;
        cJSON* jtext = cJSON_GetObjectItem(node, "text");
        cJSON* jdesc = cJSON_GetObjectItem(node, "contentDescription");
        if (!cJSON_IsString(jdesc)) jdesc = cJSON_GetObjectItem(node, "desc");
        cJSON* jcls = cJSON_GetObjectItem(node, "className");
        cJSON* jrid = cJSON_GetObjectItem(node, "resourceId");
        cJSON* jbnd = cJSON_GetObjectItem(node, "bounds");

        const char* text_s = cJSON_IsString(jtext) ? jtext->valuestring : "";
        const char* desc_s = cJSON_IsString(jdesc) ? jdesc->valuestring : "";
        n.cls = LastSegment(cJSON_IsString(jcls) ? jcls->valuestring : "", '.');
        std::string rid = LastSegment(cJSON_IsString(jrid) ? jrid->valuestring : "", '/');
        n.label = (text_s && *text_s) ? text_s : desc_s;
        if (cJSON_IsString(jbnd)) ParseBounds(jbnd->valuestring, n);

        int idx = (int)nodes.size();
        nodes.push_back(n);

        bool noisy = n.label.empty() || n.label.rfind("ref_", 0) == 0 || n.label == n.cls;
        bool interactive = (n.cls == "EditText" || n.cls == "Button" || n.cls == "ImageButton" ||
                            n.cls == "Switch" || n.cls == "CheckBox" || n.cls == "RadioButton");
        if ((!noisy || interactive) && n.has_bounds) {
            std::string label = n.label.empty() ? ("<" + n.cls + ">") : n.label;
            if (label.size() > 36) label = label.substr(0, 36);
            text += "#" + std::to_string(idx) + " " + label;
            if (!rid.empty()) text += " id=" + rid;
            if (n.has_bounds) {
                text += " " + std::to_string(n.x1) + "," + std::to_string(n.y1) + "," +
                        std::to_string(n.x2) + "," + std::to_string(n.y2);
            }
            text += "\n";
        }

        cJSON* children = cJSON_GetObjectItem(node, "children");
        if (cJSON_IsArray(children)) Walk(children, depth + 1, nodes, text);
    }
}

// 拉取并扁平化界面；返回 false 表示失败
bool FetchUi(std::string& summary, std::vector<UiNode>* nodes_out, int* screen_w, int* screen_h) {
    std::string raw;
    if (!PortalClient::GetInstance().GetState(raw)) {
        return false;
    }
    cJSON* root = cJSON_Parse(raw.c_str());
    if (!root) return false;

    cJSON* tree = cJSON_GetObjectItem(root, "a11y_tree");
    cJSON* ps = cJSON_GetObjectItem(root, "phone_state");

    std::string header;
    if (cJSON_IsObject(ps)) {
        cJSON* app = cJSON_GetObjectItem(ps, "currentApp");
        cJSON* pkg = cJSON_GetObjectItem(ps, "packageName");
        cJSON* act = cJSON_GetObjectItem(ps, "activityName");
        cJSON* kb = cJSON_GetObjectItem(ps, "keyboardVisible");
        char buf[320];
        snprintf(buf, sizeof(buf), "当前应用: %s (%s/%s)  键盘: %s\n",
                 cJSON_IsString(app) ? app->valuestring : "?",
                 cJSON_IsString(pkg) ? pkg->valuestring : "?",
                 cJSON_IsString(act) ? LastSegment(act->valuestring, '.').c_str() : "?",
                 cJSON_IsTrue(kb) ? "显示" : "隐藏");
        header = buf;
    }

    std::vector<UiNode> nodes;
    std::string text;
    Walk(tree, 0, nodes, text);
    if (text.empty()) text = "(界面上没有可读控件，可能是敏感页面或加载中)\n";

    int sw = 0, sh = 0;
    for (auto& n : nodes) {
        if (n.has_bounds) {
            if (n.x2 > sw) sw = n.x2;
            if (n.y2 > sh) sh = n.y2;
        }
    }
    if (sw < 200) sw = 1080;   // 兜底
    if (sh < 200) sh = 2400;
    s_screen_w = sw;
    s_screen_h = sh;

    summary = header + text;
    if (nodes_out) *nodes_out = nodes;
    if (screen_w) *screen_w = sw;
    if (screen_h) *screen_h = sh;
    cJSON_Delete(root);
    return true;
}

UiNode* FindByText(std::vector<UiNode>& nodes, const std::string& needle) {
    std::string want = needle;
    for (auto& c : want) c = (char)tolower((unsigned char)c);
    UiNode* partial = nullptr;
    for (auto& n : nodes) {
        if (!n.has_bounds || n.label.empty()) continue;
        std::string hay = n.label;
        for (auto& c : hay) c = (char)tolower((unsigned char)c);
        if (hay == want) return &n;
        if (!partial && hay.find(want) != std::string::npos) partial = &n;
    }
    return partial;
}

std::string Ok(const std::string& msg) {
    return "{\"success\": true, \"message\": \"" + msg + "\"}";
}
std::string Fail(const std::string& msg) {
    return "{\"success\": false, \"message\": \"" + msg + "\"}";
}

// HID 可用就用 HID 点击（不可检测），否则退回 Portal
bool TapPixel(int px, int py, int sw, int sh) {
    // 安卓的 BLE HID 主机基本不认「绝对坐标触摸数字化仪」报告，
    // 配好 Portal 时优先用无障碍点击，这是唯一可靠的坐标点击方式。
    auto& portal = PortalClient::GetInstance();
    if (portal.Configured()) {
        bool ok = portal.Tap(px, py);
        ESP_LOGI(TAG, "[动作] tap_text 走 Portal (%d,%d) → %s", px, py, ok ? "OK" : "失败");
        return ok;
    }
    int nx = sw > 0 ? px * 1000 / sw : 500;
    int ny = sh > 0 ? py * 1000 / sh : 500;
    if (PhoneHid::GetInstance().IsMounted()) {
        return PhoneHid::GetInstance().TouchTap(nx, ny);
    }
    // 不再用 IsConnected() 门槛卡住蓝牙，交给底层判断真实连接状态
    return PhoneBleHid::GetInstance().TouchTap(nx, ny);
}

// 串口诊断命令：不经过小智云端，直接驱动 Portal 全链路。
// 这样在电脑上运行 mock/computer portal 就能对「读屏 → 找控件 → 点击 → 输入」做闭环实测。
std::string ConsolePortalImpl(const std::string& cmd, const std::string& args) {
    auto& portal = PortalClient::GetInstance();

    if (cmd == "portal") {
        size_t sp = args.find(' ');
        std::string host = sp == std::string::npos ? args : args.substr(0, sp);
        std::string token = sp == std::string::npos ? std::string() : args.substr(sp + 1);
        if (host.empty()) return "用法: portal <IP[:端口]> <token>";
        bool saved = portal.Save(host, token);
        std::string out;
        bool alive = portal.Get("/version", out, 5000);
        return std::string(saved ? "已保存 " : "保存失败 ") + host +
               "；/version 探测: " + (alive ? "连通 → " + out : "失败 → " + portal.LastError());
    }

    if (!portal.Configured()) return "Portal 未配置，先发: portal 10.197.1.38 test";

    if (cmd == "screen") {
        std::string summary;
        std::vector<UiNode> nodes;
        int w = 0, h = 0;
        if (!FetchUi(summary, &nodes, &w, &h)) return "读屏失败: " + portal.LastError();
        ESP_LOGI(TAG, "[CMD:screen] 屏幕 %dx%d，控件 %u 个",
                 w, h, (unsigned)nodes.size());
        // 逐行打印，不要一次性格式化几 KB 的字符串（小栈任务会爆）
        size_t pos = 0;
        while (pos < summary.size() && pos < 4000) {
            size_t end = summary.find('\n', pos);
            if (end == std::string::npos) end = summary.size();
            ESP_LOGI(TAG, "[CMD:screen] %.*s", (int)(end - pos), summary.c_str() + pos);
            pos = end + 1;
        }
        return "读屏成功，共 " + std::to_string(nodes.size()) + " 个控件（详情见上面的日志）";
    }

    if (cmd == "click") {
        std::string summary;
        std::vector<UiNode> nodes;
        int w = 0, h = 0;
        if (!FetchUi(summary, &nodes, &w, &h)) return "读屏失败: " + portal.LastError();
        UiNode* n = FindByText(nodes, args);
        if (!n) return "没找到控件: " + args;
        int px = (n->x1 + n->x2) / 2;
        int py = (n->y1 + n->y2) / 2;
        bool ok = TapPixel(px, py, w, h);
        return "找到「" + n->label + "」中心 (" + std::to_string(px) + "," +
               std::to_string(py) + ") → " + (ok ? "点击成功" : "点击失败: " + portal.LastError());
    }

    if (cmd == "input") {
        bool ok = portal.InputText(args, true);
        return ok ? "Portal 输入成功: " + args : "输入失败: " + portal.LastError();
    }
    if (cmd == "app") {
        bool ok = portal.StartApp(args);
        return ok ? "已请求启动 " + args : "启动失败: " + portal.LastError();
    }
    if (cmd == "ptap") {
        int x = 0, y = 0;
        if (sscanf(args.c_str(), "%d %d", &x, &y) != 2) return "用法: ptap <像素x> <像素y>";
        return portal.Tap(x, y) ? "点击成功" : "点击失败: " + portal.LastError();
    }
    if (cmd == "pswipe") {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0, d = 400;
        if (sscanf(args.c_str(), "%d %d %d %d %d", &x1, &y1, &x2, &y2, &d) < 4)
            return "用法: pswipe <x1> <y1> <x2> <y2> [ms]";
        return portal.Swipe(x1, y1, x2, y2, d) ? "滑动成功" : "滑动失败: " + portal.LastError();
    }
    if (cmd == "pkey") {
        int code = atoi(args.c_str());
        return portal.SendKey(code) ? "按键成功" : "按键失败: " + portal.LastError();
    }
    return "未知 Portal 命令（支持 portal/screen/click/input/app/ptap/pswipe/pkey）";
}

}  // namespace

std::string PortalConsoleCommand(const std::string& cmd, const std::string& args) {
    return ConsolePortalImpl(cmd, args);
}

void PortalSetScreenSize(int w, int h) {
    if (w > 200 && h > 200) {
        s_screen_w = w;
        s_screen_h = h;
    }
}

void PortalGetScreenSize(int& w, int& h) {
    w = s_screen_w;
    h = s_screen_h;
}

void RegisterPortalTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.phone.screen",
        "读取安卓手机当前屏幕内容（通过无障碍服务）。返回当前应用名和界面上的文字控件清单，每行形如：\n"
        "#序号 文字 id=资源名 x1,y1,x2,y2\n"
        "用它来判断当前在哪个页面、有哪些按钮、该点哪里。\n"
        "注意：银行、支付、密码等敏感页面读不到内容（系统会屏蔽），此时应停止自动操作并告知用户。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            std::string summary;
            if (!FetchUi(summary, nullptr, nullptr, nullptr)) {
                return Fail(PortalClient::GetInstance().LastError());
            }
            if (summary.size() > 6000) summary = summary.substr(0, 6000) + "\n...(已截断)";
            return summary;
        });

    mcp.AddTool(
        "self.phone.tap_text",
        "按文字点击屏幕上的控件（最可靠的点击方式）。比如要打开“设置”就传 text=设置。\n"
        "内部会先读屏找到该文字的位置，再点击它的中心。优先用这个，而不是猜坐标。",
        PropertyList({
            Property("text", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto needle = properties["text"].value<std::string>();
            std::string summary;
            std::vector<UiNode> nodes;
            int sw = 0, sh = 0;
            if (!FetchUi(summary, &nodes, &sw, &sh)) {
                return Fail(PortalClient::GetInstance().LastError());
            }
            UiNode* hit = FindByText(nodes, needle);
            if (!hit) return Fail("当前界面没有找到文字: " + needle);
            bool ok = TapPixel(hit->cx(), hit->cy(), sw, sh);
            return ok ? Ok("已点击“" + hit->label + "”") : Fail("点击失败");
        });

    mcp.AddTool(
        "self.phone.input_text",
        "向手机当前输入框输入文本（支持中文，走输入法通道）。输入前先点一下输入框。"
        "如果是纯英文数字且 USB/蓝牙键盘已连接，也可以直接用 type 工具。",
        PropertyList({
            Property("text", kPropertyTypeString).SetMaxLength(500),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto text = properties["text"].value<std::string>();
            if (!PortalClient::GetInstance().InputText(text, true)) {
                return Fail(PortalClient::GetInstance().LastError());
            }
            return Ok("已输入 " + std::to_string(text.size()) + " 个字符");
        });

    mcp.AddTool(
        "self.phone.open_app",
        "直接启动手机上的应用。package 填包名，如微信 com.tencent.mm、设置 com.android.settings、"
        "抖音 com.ss.android.ugc.aweme。不确定包名时先用 self.phone.screen 看当前应用，或让用户确认。",
        PropertyList({
            Property("package", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto pkg = properties["package"].value<std::string>();
            if (!PortalClient::GetInstance().StartApp(pkg)) {
                return Fail(PortalClient::GetInstance().LastError());
            }
            return Ok("已尝试启动 " + pkg);
        });

    mcp.AddTool(
        "self.phone.portal_set",
        "配置手机端 Portal 的连接信息（只需设置一次，会保存到设备）。"
        "host 是手机在局域网里的 IP（可带端口，默认 8080），token 是 App 里显示的令牌。",
        PropertyList({
            Property("host", kPropertyTypeString),
            Property("token", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto host = properties["host"].value<std::string>();
            auto token = properties["token"].value<std::string>();
            if (!PortalClient::GetInstance().Save(host, token)) {
                return Fail("保存失败");
            }
            // 立刻探活：用 /version 而不是 /ping —— /ping 不需要令牌，
            // 令牌填错也会显示"连通"，白排查
            std::string out;
            bool alive = PortalClient::GetInstance().Get("/version", out, 6000);
            return alive ? Ok("已保存并连通手机 Portal（" + out + "）")
                         : Fail("已保存，但连接失败：" + PortalClient::GetInstance().LastError());
        });

    ESP_LOGI(TAG, "Portal 工具注册完成（screen / tap_text / input_text / open_app / portal_set）");
}
