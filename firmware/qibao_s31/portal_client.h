/*
 * PortalClient —— Mobilerun Portal HTTP 客户端（Android 无障碍通道）
 *
 * Portal 在手机上监听 :8080，接口：
 *   GET  /state            -> {a11y_tree, phone_state}
 *   POST /tap {x,y}
 *   POST /swipe {startX,startY,endX,endY,duration}
 *   POST /keyboard/key {key_code}
 *   POST /keyboard/input {base64_text,clear}
 *   POST /app {package}
 * 鉴权：Authorization: Bearer <token>
 */
#pragma once

#include <string>

class PortalClient {
public:
    static PortalClient& GetInstance();

    // 只改内存中的目标（用于编译期默认地址；不依赖 NVS 是否已初始化）
    void Set(const std::string& host, const std::string& token);
    // 从 NVS 读取上次保存的 host/token，没有就保留当前值
    void Load();
    // 保存配置到 NVS（host 可写 "192.168.1.5" 或 "192.168.1.5:8080"）
    bool Save(const std::string& host, const std::string& token);

    bool Configured() const { return !host_.empty(); }
    const std::string& Host() const { return host_; }
    const std::string& LastError() const { return last_error_; }

    // 底层请求：成功返回 true，out 为解包后的 result JSON 文本
    bool Get(const std::string& path, std::string& out, int timeout_ms = 8000);
    bool Post(const std::string& path, const std::string& json,
              std::string& out, int timeout_ms = 8000);

    // 高层操作
    bool GetState(std::string& out_json);
    bool Tap(int x, int y);
    bool Swipe(int x1, int y1, int x2, int y2, int duration_ms);
    bool InputText(const std::string& utf8, bool clear);
    bool SendKey(int android_keycode);
    bool StartApp(const std::string& package);

private:
    PortalClient() = default;
    bool Request(bool post, const std::string& path, const std::string& body,
                 std::string& out, int timeout_ms);
    std::string BaseUrl(const std::string& path) const;
    void SetError(const std::string& msg);

    std::string host_;
    std::string token_;
    std::string last_error_;
};
