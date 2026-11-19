#pragma once

// 七宝妹妹形象显示：继承 RgbLcdDisplay，在 SetupUI 后叠加完整全身形象图。
//
// 设计要点（按用户要求）：
//   - 用完整的全身图（站姿 qibao_standing / 坐姿 qibao_seated），头到脚完整保留，绝不裁切；
//   - 保持原比例（不拉伸变形），高度铺满屏幕，居中显示；
//   - 屏幕背景设成与原图一致的米灰渐变，左右留白自然融合，看起来像一整张铺满的图；
//   - 图片以压缩 JPG 编进固件（省 flash），启动时解码到 16MB PSRAM；
//   - **不再自动轮播**：改成手动切换 —— 点一下图片就在站姿/坐姿之间柔和淡入淡出；
//   - 左下角有一个旋转按钮，点一下把整个画面翻转 180°（这块板没有加速度计，只能手动）。

#include "display/lcd_display.h"
#include "qibao_frames.h"
#include "qibao_screen.h"

#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <lvgl.h>

#include "jpg/image_to_jpeg.h"
#include "jpg/jpeg_to_image.h"

class QibaoLcdDisplay : public RgbLcdDisplay {
public:
    QibaoLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y,
                    bool mirror_x, bool mirror_y, bool swap_xy)
        : RgbLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {}

    void SetupUI() override {
        RgbLcdDisplay::SetupUI();  // 先建好小智原生 UI（状态栏/字幕/表情区）
        s_instance_ = this;

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        // 屏幕背景设成与原图一致的米灰渐变（顶 #81796D -> 底 #CBC3B7），
        // 这样全身图两侧的留白和图片背景无缝融合，看起来像一整张铺满的图。
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x81796D), 0);
        lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0xCBC3B7), 0);
        lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        // 小智原生的 container_ 是整屏不透明白底，会把上面的渐变盖掉，
        // 两侧就变成两条白边。这里把它改成透明，渐变露出来。
        if (container_ != nullptr) {
            lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(container_, 0, 0);
        }

        // 解码两张完整全身图到 PSRAM。
        bool ok0 = DecodeFrame(qibao_frame_0_jpg, sizeof(qibao_frame_0_jpg), &dsc0_, 0);
        bool ok1 = DecodeFrame(qibao_frame_1_jpg, sizeof(qibao_frame_1_jpg), &dsc1_, 1);
        if (!ok0 && !ok1) {
            ESP_LOGE(TAG, "两张形象图都解码失败，跳过叠加");
            return;
        }

        const lv_image_dsc_t* first = ok0 ? &dsc0_ : &dsc1_;
        // 图1 也解码成功时用它做第二帧，否则退化成单张静态图
        const lv_image_dsc_t* second = (ok0 && ok1) ? &dsc1_ : nullptr;

        // 保持比例：按图片原始尺寸显示（约 264x480），比例天然保持，不拉伸变形。
        for (int i = 0; i < 2; ++i) {
            const lv_image_dsc_t* dsc = (i == 0) ? first : second;
            if (dsc == nullptr) break;
            lv_obj_t* img = lv_img_create(screen);
            lv_image_set_src(img, dsc);
            lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_TRANSP, 0);
            // 点一下图片就切换站姿/坐姿（手动，不再自动滚动）
            lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(img, OnImageClicked, LV_EVENT_CLICKED, this);
            imgs_[i] = img;
        }
        if (imgs_[0] != nullptr) lv_obj_move_foreground(imgs_[0]);
        if (imgs_[1] != nullptr) lv_obj_move_foreground(imgs_[1]);

        CreateRotateButton(screen);

        // 界面建完了，现在才能安全地应用屏幕方向（原因见 qibao_screen.h 的注释）
        qibao::ApplySavedRotation();

        if (imgs_[1] != nullptr) {
            cur_frame_ = 0;
            lv_obj_set_style_opa(imgs_[1], LV_OPA_TRANSP, 0);
            ESP_LOGI(TAG, "七宝完整全身图 x2 已显示（点图片换姿势，左下角按钮旋转画面）");
        } else {
            cur_frame_ = 0;
            ESP_LOGI(TAG, "七宝完整全身图 x1 已显示（静态）");
        }
    }

    // 手动切换站姿 / 坐姿（柔和交叉淡入淡出，约 0.6 秒）
    void SwitchPose() {
        if (imgs_[0] == nullptr || imgs_[1] == nullptr) {
            return;
        }
        int next = 1 - cur_frame_;
        lv_obj_move_foreground(imgs_[next]);
        FadeObj(imgs_[cur_frame_], LV_OPA_COVER, LV_OPA_TRANSP, 600);
        FadeObj(imgs_[next], LV_OPA_TRANSP, LV_OPA_COVER, 600);
        cur_frame_ = next;
        ESP_LOGI(TAG, "七宝换成%s", cur_frame_ == 0 ? "站姿" : "坐姿");
    }

    // 当前显示的是第几张（0=站姿 1=坐姿）
    int CurrentFrame() const { return cur_frame_; }

    // 当前实例（串口命令 qpose 用）
    static QibaoLcdDisplay* Instance() { return s_instance_; }

    // ---------------------------------------------------------------------------
    // 忠实快照：直接读「正在送显的那块缓冲」。
    //
    // 上游 LvglDisplay::SnapshotToJpeg() 用的是 lv_snapshot_take(screen, ...)，
    // 而 LVGL 9.5 的整屏快照走的是 lv_obj_redraw()（见 lv_snapshot.c）：
    //     if(top_obj == obj) lv_obj_redraw(&layer, top_obj);   // 整屏快照，变换被跳过
    //     else              lv_obj_refr(&layer, top_obj);     // 快照子对象才走变换
    // 也就是说，屏幕一旦带 transform 翻转，上游快照拍出来的还是「没翻」的画面 ——
    // 会让人误以为旋转没生效。这里改成直接读送显缓冲，拍到的就是屏幕上真实显示的内容。
    // ---------------------------------------------------------------------------
    bool SnapshotToJpeg(std::string& jpeg_data, int quality) override {
        DisplayLockGuard lock(this);
        lv_display_t* disp = lv_display_get_default();
        if (disp == nullptr) {
            ESP_LOGE(TAG, "快照失败：没有默认显示");
            return false;
        }
        lv_draw_buf_t* buf = lv_display_get_buf_active(disp);
        if (buf == nullptr || buf->data == nullptr) {
            ESP_LOGE(TAG, "快照失败：拿不到送显缓冲");
            return false;
        }
        const uint32_t w = buf->header.w;
        const uint32_t h = buf->header.h;
        const uint32_t stride_px = buf->header.stride / 2;
        if (w == 0 || h == 0 || stride_px < w) {
            ESP_LOGE(TAG, "快照失败：缓冲尺寸异常 %ux%u", (unsigned)w, (unsigned)h);
            return false;
        }

        // 拷一份紧凑的 w*h RGB565 到 PSRAM：不能就地改送显缓冲，会把屏幕搅乱
        uint16_t* copy = (uint16_t*)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        if (copy == nullptr) {
            ESP_LOGE(TAG, "快照失败：PSRAM 分配失败");
            return false;
        }
        const uint16_t* src = (const uint16_t*)buf->data;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(copy + (size_t)y * w, src + (size_t)y * stride_px, (size_t)w * 2);
        }
        // 【注意】这里不要再做 __builtin_bswap16！
        // 编码器内部对 RGB565 是直接 memcpy（软件路径用 ESP_IMGFX_PIXEL_FMT_RGB565_LE，
        // 硬件路径用 JPEG_ENCODE_IN_FORMAT_RGB565），要的就是 LVGL 原生的小端 RGB565。
        // 上游 LvglDisplay::SnapshotToJpeg() 里那次 bswap 是给「RGB565_SWAPPED」的屏用的；
        // 我们这块板子是普通 RGB565，多换一次会让颜色整个错掉（背景渐变会变成彩虹色带）。

        jpeg_data.clear();
        bool ret = image_to_jpeg_cb(
            (uint8_t*)copy, (size_t)w * h * 2, (uint16_t)w, (uint16_t)h, V4L2_PIX_FMT_RGB565, quality,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                std::string* output = static_cast<std::string*>(arg);
                if (data && len > 0) {
                    output->append(static_cast<const char*>(data), len);
                }
                return len;
            },
            &jpeg_data);
        heap_caps_free(copy);
        if (!ret) {
            ESP_LOGE(TAG, "快照失败：JPEG 编码出错");
        }
        return ret;
    }

private:
    inline static const char* TAG = "QibaoDisplay";
    inline static QibaoLcdDisplay* s_instance_ = nullptr;

    // 点图片 → 换姿势
    static void OnImageClicked(lv_event_t* e) {
        auto* self = static_cast<QibaoLcdDisplay*>(lv_event_get_user_data(e));
        if (self != nullptr) {
            self->SwitchPose();
        }
    }

    // 点旋转按钮 → 整个画面翻 180°
    static void OnRotateClicked(lv_event_t* e) {
        ESP_LOGI(TAG, "旋转按钮：%s", qibao::ToggleRotation().c_str());
    }

    // 柔和淡入淡出
    static void FadeObj(lv_obj_t* obj, lv_opa_t from, lv_opa_t to, uint32_t dur_ms) {
        if (obj == nullptr) return;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, obj);
        lv_anim_set_exec_cb(&anim, [](void* o, int32_t v) {
            lv_obj_set_style_opa(static_cast<lv_obj_t*>(o), static_cast<lv_opa_t>(v), 0);
        });
        lv_anim_set_values(&anim, from, to);
        lv_anim_set_time(&anim, dur_ms);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
    }

    // 左下角旋转按钮：纯图形（圆弧箭头），不依赖中文字体
    void CreateRotateButton(lv_obj_t* screen) {
        lv_obj_t* btn = lv_btn_create(screen);
        lv_obj_set_size(btn, 56, 56);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 12, -12);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xE8E2D6), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_radius(btn, 28, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, OnRotateClicked, LV_EVENT_CLICKED, this);

        // 圆弧 + 缺口 = 一个「循环箭头」的形状
        lv_obj_t* arc = lv_arc_create(btn);
        lv_obj_set_size(arc, 30, 30);
        lv_obj_center(arc);
        lv_arc_set_bg_angles(arc, 40, 330);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 100);
        lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(0xF0E9DC), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);  // 去掉旋钮，只留弧线
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    }

    // 解码到 PSRAM，常驻不释放（形象图一直显示）。返回是否成功。
    static bool DecodeFrame(const unsigned char* jpg, size_t len, lv_image_dsc_t* dsc, int idx) {
        uint8_t* out = nullptr;
        size_t out_len = 0, w = 0, h = 0, stride = 0;
        // max 传屏幕尺寸，解码器会按比例缩放并 round 到 8 的倍数；1080x1920 -> 约 272x480。
        esp_err_t err = jpeg_to_image_fit(jpg, len, 800, 480, &out, &out_len, &w, &h, &stride);
        if (err != ESP_OK || out == nullptr) {
            ESP_LOGE(TAG, "frame%d 解码失败: %s", idx, esp_err_to_name(err));
            return false;
        }
        dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf = LV_COLOR_FORMAT_RGB565;
        dsc->header.w = (uint32_t)w;
        dsc->header.h = (uint32_t)h;
        dsc->header.stride = (uint32_t)stride;
        dsc->data_size = out_len;
        dsc->data = out;
        ESP_LOGI(TAG, "frame%d 解码 OK: %ux%u stride=%u len=%u",
                 idx, (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned)out_len);
        return true;
    }

    lv_image_dsc_t dsc0_{};
    lv_image_dsc_t dsc1_{};
    lv_obj_t* imgs_[2] = {nullptr, nullptr};
    int cur_frame_ = 0;
};
