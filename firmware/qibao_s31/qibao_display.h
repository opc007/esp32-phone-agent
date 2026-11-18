#pragma once

// 七宝妹妹形象显示：继承 RgbLcdDisplay，在 SetupUI 后叠加完整全身形象图。
//
// 设计要点（按用户要求）：
//   - 用完整的全身图（站姿 qibao_standing / 坐姿 qibao_seated），头到脚完整保留，绝不裁切；
//   - 保持原比例（不拉伸变形），高度铺满屏幕，居中显示；
//   - 屏幕背景设成与原图一致的米灰渐变，左右留白自然融合，看起来像一整张铺满的图；
//   - 两张图做几秒一轮的柔和淡入淡出（像一段循环短视频），没有缩放/位移的机械抖动；
//   - 图片以压缩 JPG 编进固件（省 flash），启动时解码到 16MB PSRAM。

#include "display/lcd_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "jpg/jpeg_to_image.h"
#include "qibao_frames.h"

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

        // 屏幕背景设成与原图一致的米灰渐变（顶 #81796D -> 底 #CBC3B7），
        // 这样全身图两侧的留白和图片背景无缝融合，看起来像一整张铺满的图。
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x81796D), 0);
        lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0xCBC3B7), 0);
        lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        // 解码两张完整全身图到 PSRAM。
        bool ok0 = DecodeFrame(qibao_frame_0_jpg, sizeof(qibao_frame_0_jpg), &dsc0_, 0);
        bool ok1 = DecodeFrame(qibao_frame_1_jpg, sizeof(qibao_frame_1_jpg), &dsc1_, 1);
        if (!ok0 && !ok1) {
            ESP_LOGE("QibaoDisplay", "两张形象图都解码失败，跳过叠加");
            return;
        }

        const lv_image_dsc_t* first = ok0 ? &dsc0_ : &dsc1_;
        // 图1 也解码成功时用它做第二帧，否则退化成单张静态图
        const lv_image_dsc_t* second = (ok0 && ok1) ? &dsc1_ : nullptr;

        // 保持比例：以屏幕高度为基准缩放，宽度按比例算（1080x1920 -> 约 270x480）。
        // 用 lv_img 的 scale 让解码出的图铺满屏幕高度，居中。
        for (int i = 0; i < 2; ++i) {
            const lv_image_dsc_t* dsc = (i == 0) ? first : second;
            if (dsc == nullptr) break;
            lv_obj_t* img = lv_img_create(screen);
            lv_image_set_src(img, dsc);  // lv_img 按图片原始尺寸(约272x480)显示，比例天然保持
            lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_TRANSP, 0);
            imgs_[i] = img;
        }
        lv_obj_move_foreground(imgs_[0]);
        if (imgs_[1] != nullptr) lv_obj_move_foreground(imgs_[1]);

        if (imgs_[1] != nullptr) {
            // 柔和淡入淡出：图0 保持 3s -> 淡出 1.5s -> 图1 保持 3s -> 淡入 1.5s，约 9s 一轮。
            // 用两个 opacity 动画互补，ease_in_out 让过渡平滑，没有任何缩放/位移抖动。
            lv_anim_t fade0;
            lv_anim_init(&fade0);
            lv_anim_set_var(&fade0, imgs_[0]);
            lv_anim_set_exec_cb(&fade0, [](void* o, int32_t v) {
                lv_obj_set_style_opa((lv_obj_t*)o, (lv_opa_t)v, 0);
            });
            lv_anim_set_values(&fade0, LV_OPA_COVER, LV_OPA_TRANSP);
            lv_anim_set_time(&fade0, 1500);
            lv_anim_set_delay(&fade0, 3000);
            lv_anim_set_playback_time(&fade0, 1500);
            lv_anim_set_playback_delay(&fade0, 3000);
            lv_anim_set_repeat_count(&fade0, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&fade0, lv_anim_path_ease_in_out);
            lv_anim_start(&fade0);

            lv_anim_t fade1;
            lv_anim_init(&fade1);
            lv_anim_set_var(&fade1, imgs_[1]);
            lv_anim_set_exec_cb(&fade1, [](void* o, int32_t v) {
                lv_obj_set_style_opa((lv_obj_t*)o, (lv_opa_t)v, 0);
            });
            lv_anim_set_values(&fade1, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_time(&fade1, 1500);
            lv_anim_set_delay(&fade1, 3000);
            lv_anim_set_playback_time(&fade1, 1500);
            lv_anim_set_playback_delay(&fade1, 3000);
            lv_anim_set_repeat_count(&fade1, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&fade1, lv_anim_path_ease_in_out);
            lv_anim_start(&fade1);

            ESP_LOGI("QibaoDisplay", "七宝完整全身图 x2 已显示 + 淡入淡出循环（无抖动）");
        } else {
            ESP_LOGI("QibaoDisplay", "七宝完整全身图 x1 已显示（静态）");
        }
    }

private:
    // 解码到 PSRAM，常驻不释放（形象图一直显示）。返回是否成功。
    static bool DecodeFrame(const unsigned char* jpg, size_t len, lv_image_dsc_t* dsc, int idx) {
        uint8_t* out = nullptr;
        size_t out_len = 0, w = 0, h = 0, stride = 0;
        // max 传屏幕尺寸，解码器会按比例缩放并 round 到 8 的倍数；1080x1920 -> 约 272x480。
        esp_err_t err = jpeg_to_image_fit(jpg, len, 800, 480, &out, &out_len, &w, &h, &stride);
        if (err != ESP_OK || out == nullptr) {
            ESP_LOGE("QibaoDisplay", "frame%d 解码失败: %s", idx, esp_err_to_name(err));
            return false;
        }
        dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf = LV_COLOR_FORMAT_RGB565;
        dsc->header.w = (uint32_t)w;
        dsc->header.h = (uint32_t)h;
        dsc->header.stride = (uint32_t)stride;
        dsc->data_size = out_len;
        dsc->data = out;
        ESP_LOGI("QibaoDisplay", "frame%d 解码 OK: %ux%u stride=%u len=%u",
                 idx, (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned)out_len);
        return true;
    }

    lv_image_dsc_t dsc0_{};
    lv_image_dsc_t dsc1_{};
    lv_obj_t* imgs_[2] = {nullptr, nullptr};
};
