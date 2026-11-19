/*
 * qibao_screen.cc —— 七宝的屏幕方向管理（手动翻转，四个挡）
 *
 * 原理说明见 qibao_screen.h 的注释，这里只讲实现细节。
 *
 * 这一层只管「现在是第几挡」这状态 + 存 NVS + 通知显示层；
 * 真正把画面转过来的活在 qibao_display.h 的布局根容器上。
 */
#include "qibao_screen.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace qibao {
namespace {

const char* TAG = "QibaoScreen";

constexpr const char* kNvsNamespace = "qibao_ui";
constexpr const char* kNvsKeyRot = "rot";       // 新键：存 0.1 度角度（0/900/1800/2700）
constexpr const char* kNvsKeyRot180 = "rot180"; // 旧键：只存 0/1，升级时迁移一下

lv_display_t* s_disp = nullptr;
Rotation s_rotation = Rotation::kNormal;
bool s_loaded = false;
RotationChangedCallback s_rotation_cb = nullptr;
lv_obj_t* s_layout_root = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
PhysicalTapCallback s_tap_cb = nullptr;
bool s_touch_down = false;

void TouchPollCb(lv_timer_t*) {
    if (s_touch == nullptr) {
        return;
    }
    const esp_err_t read_err = esp_lcd_touch_read_data(s_touch);
    if (read_err != ESP_OK) {
        static bool logged = false;
        if (!logged) {
            ESP_LOGW(TAG, "读 GT1151 失败: %s", esp_err_to_name(read_err));
            logged = true;
        }
        return;
    }
    esp_lcd_touch_point_data_t point = {};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) != ESP_OK) {
        return;
    }
    const bool down = count > 0;
    if (down && !s_touch_down) {
        ESP_LOGI(TAG, "GT1151 按下 x=%u y=%u", point.x, point.y);
        if (s_tap_cb != nullptr) {
            s_tap_cb(point.x, point.y);
        }
    }
    s_touch_down = down;
}

// 翻转后要补画几次整屏（见 ScheduleFullRedraw 里的说明）
int s_full_redraw_left = 0;

// ---------------------------------------------------------------------------
// NVS 存取
// ---------------------------------------------------------------------------
bool ReadSaved(Rotation* out) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    int32_t value = 0;
    esp_err_t err = nvs_get_i32(handle, kNvsKeyRot, &value);
    if (err != ESP_OK) {
        // 新键没有 -> 迁移旧键（旧版本只存了 0/1 两种）
        err = nvs_get_i32(handle, kNvsKeyRot180, &value);
        if (err == ESP_OK && value == 1) {
            *out = Rotation::kUpsideDown;
            nvs_close(handle);
            return true;
        }
        nvs_close(handle);
        return false;
    }
    nvs_close(handle);

    switch (value) {
        case 0:   *out = Rotation::kNormal; return true;
        case 900: *out = Rotation::kPortrait; return true;
        case 1800:*out = Rotation::kUpsideDown; return true;
        case 2700:*out = Rotation::kPortraitAlt; return true;
        default: return false;
    }
}

void SaveRotation(Rotation rotation) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_i32(handle, kNvsKeyRot, (int32_t)rotation);
    nvs_commit(handle);
    nvs_close(handle);
}

// 延迟到第一次用到方向时再读 NVS，避免「谁先谁后」的坑
void EnsureLoaded() {
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    Rotation saved = Rotation::kNormal;
    if (ReadSaved(&saved)) {
        s_rotation = saved;
        ESP_LOGI(TAG, "恢复上次保存的方向：%s", RotationName().c_str());
    }
}

// ---------------------------------------------------------------------------
// 整屏补画：DIRECT 模式 + 双缓冲的坑
// ---------------------------------------------------------------------------
void FullRedrawCb(lv_timer_t* timer) {
    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
    }
    if (--s_full_redraw_left <= 0) {
        lv_timer_delete(timer);
    }
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

// 标记块：用来肉眼+数值双重确认画面到底转没转。
// 放在**布局根容器**的左上角，这样它会跟着界面一起转 ——
// 转到哪个角，就说明当前挡位是对的。
lv_obj_t* s_marker = nullptr;

// 延时回调：等这一帧画完之后，在送显缓冲里全屏扫描洋红色，
// 报出它的包围盒位置。全扫比只看四角靠谱：四角可能被别的控件盖住。
void MarkerCheckCb(lv_timer_t* timer) {
    if (s_disp != nullptr) {
        lv_draw_buf_t* buf = lv_display_get_buf_active(s_disp);
        if (buf != nullptr && buf->data != nullptr) {
            uint32_t w = buf->header.w;
            uint32_t h = buf->header.h;
            uint32_t sp = buf->header.stride / 2;
            if (w > 32 && h > 32 && sp >= w) {
                const uint16_t* d = (const uint16_t*)buf->data;
                uint32_t lt = d[20 * sp + 20];              // 左上
                uint32_t rt = d[20 * sp + (w - 21)];        // 右上
                uint32_t lb = d[(h - 21) * sp + 20];        // 左下
                uint32_t rb = d[(h - 21) * sp + (w - 21)];  // 右下
                ESP_LOGI(TAG, "[标记] 缓冲 %ux%u stride=%u", (unsigned)w, (unsigned)h, (unsigned)sp);
                ESP_LOGI(TAG, "[标记] 左上=%04x 右上=%04x 左下=%04x 右下=%04x", lt, rt, lb, rb);

                // 全屏扫描洋红（0xF81F），找它的包围盒
                int min_x = (int)w, min_y = (int)h, max_x = -1, max_y = -1;
                uint32_t hits = 0;
                for (uint32_t y = 0; y < h; ++y) {
                    const uint16_t* row = d + (size_t)y * sp;
                    for (uint32_t x = 0; x < w; ++x) {
                        if (row[x] == 0xF81F) {
                            ++hits;
                            if ((int)x < min_x) min_x = (int)x;
                            if ((int)x > max_x) max_x = (int)x;
                            if ((int)y < min_y) min_y = (int)y;
                            if ((int)y > max_y) max_y = (int)y;
                        }
                    }
                }
                if (hits == 0) {
                    ESP_LOGI(TAG, "[标记] 整屏没扫到洋红标记（当前方向 %s）", RotationName().c_str());
                } else {
                    ESP_LOGI(TAG, "[标记] 洋红命中 %u 像素，包围盒=(%d,%d)-(%d,%d)，中心=(%d,%d)",
                             (unsigned)hits, min_x, min_y, max_x, max_y,
                             (min_x + max_x) / 2, (min_y + max_y) / 2);
                    // 期望：标记在布局根容器左上角，旋转后落在屏幕的某个角
                    const char* where = "位置不对";
                    int cx = (min_x + max_x) / 2;
                    int cy = (min_y + max_y) / 2;
                    if (cx < w / 4 && cy < h / 4) where = "左上 => 0°（正向）";
                    else if (cx > w * 3 / 4 && cy < h / 4) where = "右上 => 90°（顺时针）";
                    else if (cx > w * 3 / 4 && cy > h * 3 / 4) where = "右下 => 180°（倒置）";
                    else if (cx < w / 4 && cy > h * 3 / 4) where = "左下 => 270°（逆时针）";
                    ESP_LOGI(TAG, "[标记] 标记在%s，当前方向 %s", where, RotationName().c_str());
                }
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
void DumpTransformStateInternal() {
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        ESP_LOGW(TAG, "[诊断] 没有活动屏幕");
        return;
    }
    lv_obj_t* target = (s_layout_root != nullptr) ? s_layout_root : screen;
    const int32_t rot = lv_obj_get_style_transform_rotation(target, LV_PART_MAIN);
    const int32_t px = lv_obj_get_style_transform_pivot_x(target, LV_PART_MAIN);
    const int32_t py = lv_obj_get_style_transform_pivot_y(target, LV_PART_MAIN);
    const uint32_t children = lv_obj_get_child_count(target);
    lv_area_t coords;
    lv_obj_get_coords(target, &coords);

    ESP_LOGI(TAG, "[诊断] 变换对象=%p%s 子对象数=%u 坐标=(%d,%d)-(%d,%d)",
             (void*)target, (target == screen) ? "(屏幕)" : "(布局根容器)", (unsigned)children,
             (int)coords.x1, (int)coords.y1, (int)coords.x2, (int)coords.y2);
    ESP_LOGI(TAG, "[诊断] transform_rotation=%d pivot=(%d,%d) 当前方向 %s",
             (int)rot, (int)px, (int)py, RotationName().c_str());

    // 逐个列出直接子对象，看谁持有变换样式
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t* child = lv_obj_get_child(target, (int32_t)i);
        if (child == nullptr) continue;
        const int32_t crot = lv_obj_get_style_transform_rotation(child, LV_PART_MAIN);
        ESP_LOGI(TAG, "[诊断]   子对象[%u]=%p rotation=%d 子对象数=%u",
                 (unsigned)i, (void*)child, (int)crot,
                 (unsigned)lv_obj_get_child_count(child));
    }
}

// 只在 LVGL 线程里跑：改显示状态属于渲染状态，不能从别的线程直接动。
// 特别注意：不能在别的线程调 lv_refr_now()，会和 LVGL 线程互等，死锁。
void ApplyLvglCb(void* /*arg*/) {
    if (s_rotation_cb != nullptr) {
        s_rotation_cb(s_rotation);
    } else {
        ESP_LOGW(TAG, "还没有显示层注册方向回调，只记下了状态");
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

// 标记测试：往布局根容器的左上角放一个洋红方块，等一帧后读四角像素。
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
            lv_obj_t* target = (s_layout_root != nullptr) ? s_layout_root : screen;
            ESP_LOGI(TAG, "[标记] 变换前 rotation=%d pivot=(%d,%d) 子对象数=%u 方向=%s",
                     (int)lv_obj_get_style_transform_rotation(target, LV_PART_MAIN),
                     (int)lv_obj_get_style_transform_pivot_x(target, LV_PART_MAIN),
                     (int)lv_obj_get_style_transform_pivot_y(target, LV_PART_MAIN),
                     (unsigned)lv_obj_get_child_count(target), RotationName().c_str());

            if (s_marker == nullptr) {
                s_marker = lv_obj_create(target);
                lv_obj_set_size(s_marker, 60, 60);
                lv_obj_align(s_marker, LV_ALIGN_TOP_LEFT, 0, 0);
                lv_obj_set_style_bg_color(s_marker, lv_color_hex(0xFF00FF), 0);
                lv_obj_set_style_border_width(s_marker, 0, 0);
                lv_obj_set_style_radius(s_marker, 0, 0);
                lv_obj_set_style_opa(s_marker, LV_OPA_COVER, 0);
                lv_obj_move_foreground(s_marker);
            }
            lv_obj_invalidate(target);
            // 再把整屏刷一遍：竖屏时布局根容器有一半在屏幕外，
            // 只靠局部队列刷新可能刷不到变换图层（实测 90° 挡标记没画出来）。
            // 整屏重画会把变换图层整个重绘，标记肯定在里面；
            // 顺带把面板的两块帧缓冲都补上（DIRECT 双缓冲）。
            lv_obj_invalidate(lv_screen_active());
            ScheduleFullRedraw();
            lv_timer_create(MarkerCheckCb, 1200, nullptr);
        },
        nullptr);
}

// 读当前送显缓冲的采样校验和（调试用：验证旋转真的作用到了送显内容上）
void ProbeFrame() {
    ProbeFrameInternal();
}

void SetPhysicalTapCallback(PhysicalTapCallback cb) {
    s_tap_cb = cb;
}

void InitScreen(esp_lcd_touch_handle_t touch) {
    s_disp = lv_display_get_default();
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "没有默认显示，屏幕方向功能不可用");
    }
    EnsureLoaded();
    // 直接轮询触摸芯片。LVGL 的点击层跟着布局旋转后，热区和手指对不上，
    // 点屏幕不会进对话。这里不靠那层控件，手指按下就通知显示层。
    if (touch != nullptr && s_touch == nullptr) {
        s_touch = touch;
        lv_timer_create(TouchPollCb, 20, nullptr);
        ESP_LOGI(TAG, "触摸轮询已开：轻点屏幕即和七宝对话");
    }
}

void ScheduleFullRedraw() {
    // 【关键】DIRECT 模式 + 双缓冲的坑：
    // 这块板 esp_lvgl_port 开了 avoid_tearing，LVGL 直接拿面板自己的两块帧缓冲当绘制缓冲，
    // 渲染完一块面板就切到另一块（lv_refr.c:1404 的 buf_act 交换）。
    // 而 LVGL 每帧**只重画失效区**，所以：
    //   翻转后第一次整屏重画只落到其中一块缓冲，另一块还留着翻转前的旧画面；
    //   面板两块轮换着扫出来 —— 屏幕就在「翻了 / 没翻」之间来回跳，
    //   看起来就是「按了旋转但画面转不过来」。
    // 这里连着补几次整屏重画，让两块缓冲都变成翻转后的内容；
    // 之后的小范围刷新只会往里面覆盖一小块翻转后的画面，状态就稳住了。
    if (s_disp == nullptr) {
        return;
    }
    s_full_redraw_left = 4;
    lv_timer_create(FullRedrawCb, 60, nullptr);
}

void SetRotationChangedCallback(RotationChangedCallback cb) {
    s_rotation_cb = cb;
}

void SetLayoutRootHandle(lv_obj_t* root) {
    s_layout_root = root;
}

Rotation CurrentRotation() {
    EnsureLoaded();
    return s_rotation;
}

int CurrentAngleDeg() {
    return (int)CurrentRotation() / 10;
}

bool IsPortrait() {
    const int deg = CurrentAngleDeg();
    return deg == 90 || deg == 270;
}

std::string RotationName() {
    switch (CurrentRotation()) {
        case Rotation::kPortrait:  return "90° 竖屏（设备逆时针拿）";
        case Rotation::kUpsideDown:  return "180° 横屏倒置";
        case Rotation::kPortraitAlt: return "270° 竖屏（设备顺时针拿）";
        default:                     return "0° 横屏正向";
    }
}

bool SetRotation(Rotation rotation) {
    if (s_disp == nullptr) {
        return false;
    }
    s_loaded = true;
    s_rotation = rotation;
    SaveRotation(rotation);

    // 显示层重建界面属于渲染状态，必须交给 LVGL 线程
    lv_async_call(ApplyLvglCb, nullptr);

    ESP_LOGI(TAG, "屏幕方向: %s", RotationName().c_str());
    return true;
}

std::string ToggleRotation() {
    if (s_disp == nullptr) {
        return "屏幕方向功能不可用（显示未初始化）";
    }
    // 按一次换到下一个方向：0° -> 90° -> 180° -> 270° -> 0°，
    // 用户可以一直按到满意为止。
    Rotation next = Rotation::kNormal;
    switch (CurrentRotation()) {
        case Rotation::kNormal:      next = Rotation::kPortrait; break;
        case Rotation::kPortrait:  next = Rotation::kUpsideDown; break;
        case Rotation::kUpsideDown:  next = Rotation::kPortraitAlt; break;
        default:                     next = Rotation::kNormal; break;
    }
    SetRotation(next);
    return RotationName();
}

}  // namespace qibao
