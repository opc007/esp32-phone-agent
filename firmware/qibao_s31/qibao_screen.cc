/*
 * qibao_screen.cc —— 七宝的屏幕方向管理（手动翻转）
 *
 * 原理说明见 qibao_screen.h 的注释，这里只讲实现细节。
 */
#include "qibao_screen.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace qibao {
namespace {

const char* TAG = "QibaoScreen";

constexpr const char* kNvsNamespace = "qibao_ui";
constexpr const char* kNvsKeyRot180 = "rot180";

lv_display_t* s_disp = nullptr;
Rotation s_rotation = Rotation::kNormal;

// 翻转后要补画几次整屏（见 ApplyInLvglThread 里的说明）
int s_full_redraw_left = 0;

// 定时补一次整屏重画：让面板的两块帧缓冲都变成翻转后的内容
void FullRedrawCb(lv_timer_t* timer) {
    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
    }
    if (--s_full_redraw_left <= 0) {
        lv_timer_delete(timer);
    }
}

bool ReadRotated() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    int32_t value = 0;
    esp_err_t err = nvs_get_i32(handle, kNvsKeyRot180, &value);
    nvs_close(handle);
    return err == ESP_OK && value == 1;
}

void SaveRotated(bool rotated) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_i32(handle, kNvsKeyRot180, rotated ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);
}

// 只在 LVGL 线程里跑：改显示旋转属于渲染状态，不能从别的线程直接动。
// 特别注意：不能在别的线程调 lv_refr_now()，会和 LVGL 线程互等，死锁。
void ApplyInLvglThread() {
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        return;
    }

    // 【关键】显式把变换中心设到屏幕正中（50%/50%）。
    // LVGL 的 transform_pivot 默认值是 0，也就是对象左上角。
    // 不设的话 180° 绕左上角旋转，整个画面会平移出屏幕 ——
    // 表现出来就是「按了旋转按钮，画面却转不过来」。
    lv_obj_set_style_transform_pivot_x(screen, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(screen, LV_PCT(50), 0);

    // 整屏翻转用「对象级变换」实现：
    // LVGL 会把带 transform 的对象渲染到独立图层，再把图层旋转着画上去。
    // 这是纯软件渲染器（LV_DRAW_SW）唯一支持的路子；
    // 千万别改用 lv_display_set_rotation + 矩阵 —— 矩阵开关一开，
    // LVGL 就走矩阵那条路，而软件渲染器不消费矩阵，画面根本不会转。
    // 角度单位是 0.1 度：1800 = 180°。
    const int32_t angle = (s_rotation == Rotation::kUpsideDown) ? 1800 : 0;
    lv_obj_set_style_transform_rotation(screen, angle, 0);

    lv_obj_invalidate(screen);

    // 【关键】DIRECT 模式 + 双缓冲的坑：
    // 这块板 esp_lvgl_port 开了 avoid_tearing，LVGL 直接拿面板自己的两块帧缓冲当绘制缓冲，
    // 渲染完一块面板就切到另一块（lv_refr.c:1404 的 buf_act 交换）。
    // 而 LVGL 每帧**只重画失效区**，所以：
    //   翻转后第一次整屏重画只落到其中一块缓冲，另一块还留着翻转前的旧画面；
    //   面板两块轮换着扫出来 —— 屏幕就在「翻了 / 没翻」之间来回跳，
    //   看起来就是「按了旋转但画面转不过来」。
    // 这里连着补几次整屏重画，让两块缓冲都变成翻转后的内容；
    // 之后的小范围刷新只会往里面覆盖一小块翻转后的画面，状态就稳住了。
    s_full_redraw_left = 4;
    lv_timer_create(FullRedrawCb, 60, nullptr);
}

void ApplyLvglCb(void* /*arg*/) {
    ApplyInLvglThread();
}

// ---------------------------------------------------------------------------
// 探针：直接读「正在送显的那块缓冲」（direct 模式下就是面板自己的帧缓冲），
// 算两个校验和：一个按正序取样，一个按 180° 旋转序取样。
// 正常显示时两者不同；把画面翻转 180° 之后，两者应该互换 ——
// 这就证明旋转真的作用到了送显内容上，而不是只改了标志位。
// ---------------------------------------------------------------------------
void ProbeFrameInternal() {
    if (s_disp == nullptr) {
        return;
    }
    lv_draw_buf_t* buf = lv_display_get_buf_active(s_disp);
    if (buf == nullptr || buf->data == nullptr) {
        ESP_LOGW(TAG, "[探针] 拿不到当前送显缓冲");
        return;
    }
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride_px = buf->header.stride / 2;
    if (w == 0 || h == 0 || stride_px < w) {
        ESP_LOGW(TAG, "[探针] 缓冲尺寸异常 %ux%u", w, h);
        return;
    }
    const uint16_t* data = (const uint16_t*)buf->data;
    uint32_t sum_fwd = 0;
    uint32_t sum_rev = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t x = (i * 977u) % w;
        uint32_t y = (i * 613u) % h;
        uint32_t rx = w - 1 - x;
        uint32_t ry = h - 1 - y;
        sum_fwd = sum_fwd * 31u + data[y * stride_px + x];
        sum_rev = sum_rev * 31u + data[ry * stride_px + rx];
    }
    ESP_LOGI(TAG, "[探针] 送显缓冲 %ux%u  正序=%08x  旋转序=%08x",
             w, h, sum_fwd, sum_rev);
}

void Apply(Rotation rotation) {
    s_rotation = rotation;

    // 触摸坐标不用管：LVGL 命中判定自带逆变换
    // （lv_indev_search_obj -> lv_obj_transform_point(INVERSE)），
    // 会把触摸点反变换回对象坐标，天然「点哪就是哪」。
    // 之前在这里调 esp_lcd_touch_set_mirror_x/y 是双重翻转，点了左边会触发右边。

    // 显示旋转必须交给 LVGL 线程
    lv_async_call(ApplyLvglCb, nullptr);

    ESP_LOGI(TAG, "屏幕方向: %s", RotationName().c_str());
}

// 标记块：用来肉眼+数值双重确认画面到底转没转。
// 在屏幕左上角放一个洋红色方块，如果 180° 变换生效，它应该出现在右下角。
lv_obj_t* s_marker = nullptr;

// 延时回调：等这一帧画完之后，读送显缓冲的四个角。
void MarkerCheckCb(lv_timer_t* timer) {
    if (s_disp != nullptr) {
        lv_draw_buf_t* buf = lv_display_get_buf_active(s_disp);
        if (buf != nullptr && buf->data != nullptr) {
            uint32_t w = buf->header.w;
            uint32_t h = buf->header.h;
            uint32_t sp = buf->header.stride / 2;
            if (w > 32 && h > 32 && sp >= w) {
                const uint16_t* d = (const uint16_t*)buf->data;
                uint32_t mx = 20, my = 20;              // 标记原始位置（左上）
                uint32_t rx = w - 21, ry = h - 21;      // 180° 旋转后的对应位置（右下）
                ESP_LOGI(TAG, "[标记] 缓冲 %ux%u stride=%u", (unsigned)w, (unsigned)h, (unsigned)sp);
                ESP_LOGI(TAG, "[标记] 左上(20,20)=%04x   右下(%u,%u)=%04x",
                         d[my * sp + mx], (unsigned)rx, (unsigned)ry, d[ry * sp + rx]);
                ESP_LOGI(TAG, "[标记] %s",
                         d[my * sp + mx] == 0xF81F ? "标记在左上 => 没有旋转"
                                                   : (d[ry * sp + rx] == 0xF81F ? "标记在右下 => 旋转生效"
                                                                              : "标记不在四角 => 画面被别的东西盖住了"));
            } else {
                ESP_LOGW(TAG, "[标记] 缓冲尺寸异常 %ux%u", (unsigned)w, (unsigned)h);
            }
        }
    }
    if (s_marker != nullptr) {
        lv_obj_del(s_marker);
        s_marker = nullptr;
    }
    if (timer != nullptr) {
        lv_timer_delete(timer);
    }
}

// 诊断：把 LVGL 内部与变换相关的状态原样打出来。
// 用来确认「样式设上了没有」「图层类型变了没有」「变换层有没有内容」。
void DumpTransformStateInternal() {
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        ESP_LOGW(TAG, "[诊断] 没有活动屏幕");
        return;
    }
    const int32_t rot = lv_obj_get_style_transform_rotation(screen, LV_PART_MAIN);
    const int32_t px = lv_obj_get_style_transform_pivot_x(screen, LV_PART_MAIN);
    const int32_t py = lv_obj_get_style_transform_pivot_y(screen, LV_PART_MAIN);
    const uint32_t children = lv_obj_get_child_count(screen);
    lv_area_t coords;
    lv_obj_get_coords(screen, &coords);

    ESP_LOGI(TAG, "[诊断] screen=%p 子对象数=%u 坐标=(%d,%d)-(%d,%d)",
             (void*)screen, (unsigned)children,
             (int)coords.x1, (int)coords.y1, (int)coords.x2, (int)coords.y2);
    ESP_LOGI(TAG, "[诊断] transform_rotation=%d pivot=(%d,%d)", (int)rot, (int)px, (int)py);

    // 逐个列出 screen 的直接子对象，看谁持有变换样式
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t* child = lv_obj_get_child(screen, (int32_t)i);
        if (child == nullptr) continue;
        const int32_t crot = lv_obj_get_style_transform_rotation(child, LV_PART_MAIN);
        ESP_LOGI(TAG, "[诊断]   子对象[%u]=%p rotation=%d 子对象数=%u",
                 (unsigned)i, (void*)child, (int)crot,
                 (unsigned)lv_obj_get_child_count(child));
    }
}

}  // namespace

// 诊断入口（可在任意线程调用，内部切到 LVGL 线程执行）
void DumpTransformState() {
    lv_async_call(
        [](void*) {
            DumpTransformStateInternal();
        },
        nullptr);
}

// 标记测试：放一个洋红方块到屏幕左上角，等一帧后读四角像素。
// 这是「画面到底转没转」的决定性判据 —— 比整屏校验和可靠得多
// （激活页文字一直在变，会把校验和搅乱）。
void RunMarkerTest() {
    lv_async_call(
        [](void*) {
            lv_obj_t* screen = lv_screen_active();
            if (screen == nullptr) {
                ESP_LOGW(TAG, "[标记] 没有活动屏幕");
                return;
            }
            ESP_LOGI(TAG, "[标记] 变换前 rotation=%d pivot=(%d,%d) 子对象数=%u",
                     (int)lv_obj_get_style_transform_rotation(screen, LV_PART_MAIN),
                     (int)lv_obj_get_style_transform_pivot_x(screen, LV_PART_MAIN),
                     (int)lv_obj_get_style_transform_pivot_y(screen, LV_PART_MAIN),
                     (unsigned)lv_obj_get_child_count(screen));

            if (s_marker == nullptr) {
                s_marker = lv_obj_create(screen);
                lv_obj_set_size(s_marker, 60, 60);
                lv_obj_align(s_marker, LV_ALIGN_TOP_LEFT, 0, 0);
                lv_obj_set_style_bg_color(s_marker, lv_color_hex(0xFF00FF), 0);
                lv_obj_set_style_border_width(s_marker, 0, 0);
                lv_obj_set_style_radius(s_marker, 0, 0);
                lv_obj_set_style_opa(s_marker, LV_OPA_COVER, 0);
                lv_obj_move_foreground(s_marker);
            }
            lv_obj_invalidate(screen);
            lv_timer_create(MarkerCheckCb, 800, nullptr);
        },
        nullptr);
}


// 读当前送显缓冲的采样校验和（调试用：验证旋转真的作用到了送显内容上）
void ProbeFrame() {
    ProbeFrameInternal();
}

void InitScreen(esp_lcd_touch_handle_t touch) {
    // touch 句柄保留在签名里是为了以后要用；当前旋转方案不需要驱动层镜像。
    (void)touch;
    s_disp = lv_display_get_default();
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "没有默认显示，屏幕方向功能不可用");
    }
    // 注意：这里**不要**直接应用方向。
    // 见 ApplySavedRotation() 的注释：屏幕内容还没建的时候设 transform 是无效的。
}

void ApplySavedRotation() {
    if (s_disp == nullptr) {
        return;
    }
    Apply(ReadRotated() ? Rotation::kUpsideDown : Rotation::kNormal);
}

bool SetRotation(Rotation rotation) {
    if (s_disp == nullptr) {
        return false;
    }
    Apply(rotation);
    SaveRotated(rotation == Rotation::kUpsideDown);
    return true;
}

std::string ToggleRotation() {
    if (s_disp == nullptr) {
        return "屏幕方向功能不可用（显示未初始化）";
    }
    // 按一次换到下一个方向：0° <-> 180°，用户可以一直按到满意为止。
    const Rotation next = (s_rotation == Rotation::kNormal) ? Rotation::kUpsideDown
                                                             : Rotation::kNormal;
    SetRotation(next);
    return RotationName();
}

Rotation CurrentRotation() {
    return s_rotation;
}

bool IsRotated() {
    return s_rotation == Rotation::kUpsideDown;
}

std::string RotationName() {
    return s_rotation == Rotation::kUpsideDown ? "180° 倒置" : "0° 正向";
}

}  // namespace qibao
