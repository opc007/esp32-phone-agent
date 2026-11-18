#pragma once

// 七宝妹妹形象显示：继承 RgbLcdDisplay，在 SetupUI 后叠加一张形象图，
// 并加上「呼吸缩放 + 上下浮动」LVGL 动画，让七宝看起来是活的。
// 形象图资源由 tools/jpg2lvgl.py 从 assets/qibao_face480.jpg 生成。

#include "display/lcd_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <lvgl.h>

#include "qibao_avatar.h"

class QibaoLcdDisplay : public RgbLcdDisplay {
public:
    QibaoLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y,
                    bool mirror_x, bool mirror_y, bool swap_xy)
        : RgbLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {}

    void SetupUI() override {
        RgbLcdDisplay::SetupUI();  // 先建好小智原生 UI（状态栏/字幕/表情区）

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        // 形象图放在 emoji_box_ 之上、top_bar_ 之下。用 lv_obj_move_foreground
        // 提到最前，避免被后续控件盖住；top_bar_ 是半透明状态栏，压在图上没关系。
        avatar_ = lv_img_create(screen);
        lv_image_set_src(avatar_, &qibao_avatar);
        lv_obj_set_size(avatar_, AVATAR_SIZE, AVATAR_SIZE);
        lv_obj_align(avatar_, LV_ALIGN_CENTER, 0, AVATAR_Y_OFFSET);
        lv_obj_set_style_image_recolor_opa(avatar_, LV_OPA_TRANSP, 0);
        lv_obj_move_foreground(avatar_);

        // 呼吸动画：缩放 96% <-> 104%，2.4s 一个来回，无限循环、 easing 平滑。
        lv_anim_t anim_scale;
        lv_anim_init(&anim_scale);
        lv_anim_set_var(&anim_scale, avatar_);
        lv_anim_set_exec_cb(&anim_scale, [](void* obj, int32_t v) {
            lv_obj_set_style_transform_scale((lv_obj_t*)obj, v, 0);
        });
        lv_anim_set_values(&anim_scale, 96, 104);
        lv_anim_set_time(&anim_scale, 2400);
        lv_anim_set_playback_time(&anim_scale, 2400);
        lv_anim_set_repeat_count(&anim_scale, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim_scale, lv_anim_path_ease_in_out);
        lv_anim_start(&anim_scale);

        // 浮动动画：Y 方向 ±6px 缓慢上下，3.6s 一个来回，和呼吸错开周期。
        lv_anim_t anim_float;
        lv_anim_init(&anim_float);
        lv_anim_set_var(&anim_float, avatar_);
        lv_anim_set_exec_cb(&anim_float, [](void* obj, int32_t v) {
            lv_obj_set_y((lv_obj_t*)obj, v);
        });
        lv_anim_set_values(&anim_float, AVATAR_Y_OFFSET - 6, AVATAR_Y_OFFSET + 6);
        lv_anim_set_time(&anim_float, 3600);
        lv_anim_set_playback_time(&anim_float, 3600);
        lv_anim_set_repeat_count(&anim_float, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim_float, lv_anim_path_ease_in_out);
        lv_anim_start(&anim_float);

        ESP_LOGI("QibaoDisplay", "七宝形象图已显示（%dx%d）+ 呼吸/浮动动画",
                 AVATAR_SIZE, AVATAR_SIZE);
    }

private:
    static constexpr int AVATAR_SIZE = 360;
    static constexpr int AVATAR_Y_OFFSET = -10;  // 略微上移，给底部字幕留空间
    lv_obj_t* avatar_ = nullptr;
};
