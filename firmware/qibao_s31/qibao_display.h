#pragma once

// 七宝妹妹形象显示：继承 RgbLcdDisplay，支持 0°/90°/180°/270° 四挡屏幕方向。
//
// 设计要点（按用户要求）：
//   - 用完整的全身图（站姿 qibao_standing / 坐姿 qibao_seated），头到脚完整保留，绝不裁切；
//   - 保持原比例（不拉伸变形），高度铺满屏幕，居中显示；
//   - 屏幕背景设成与原图一致的米灰渐变，左右留白自然融合，看起来像一整张铺满的图；
//   - 图片以压缩 JPG 编进固件（省 flash），启动时解码到 PSRAM；
//   - 轻点画面任意处（除左下角小旋转按钮）就和七宝开始/结束对话，不用找板子上的按键；
//   - 左下角有一个旋转按钮，点一下转 90°（这块板没有加速度计，只能手动）。
//   - 换站姿/坐姿仍可用串口 qpose，不再占用轻点。
//
// ---------------------------------------------------------------------------
// 四挡方向是怎么做的（核心原理见 qibao_screen.h 的注释）
//
// 物理屏是 800x480 横屏。为了能「竖屏看内容」，做法是：
//   - 把整套界面塞进一个「布局根容器」；
//   - 横屏挡（0°/180°）：容器 800x480，正好铺满屏幕；
//   - 竖屏挡（90°/270°）：容器 480x800，绕屏幕中心转 90° 后正好铺满 800x480；
//   - 对容器做 lv_obj_set_style_transform_rotation()。
//
// 为什么不用「显示级旋转」（lv_display_set_rotation）：esp_lvgl_port 的触摸代码
// 不支持显示级旋转，旋转后触摸全错位；容器级变换的触摸逆变换是 LVGL 自带的，
// 实测「点哪就是哪」，局部刷新（聊天文字更新）也落在正确位置。
//
// 横屏挡和竖屏挡的布局尺寸不同，所以换挡时要**重建界面**：
// 人物图的解码结果按朝向分别缓存（横屏一套、竖屏一套），
// 重建时直接复用，不重新解码（每张 720KB，解码要几百毫秒）。

#include "display/lcd_display.h"
#include "qibao_frames.h"
#include "qibao_screen.h"
#include "qibao_persona.h"
#include "application.h"

#include <cstring>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "jpg/image_to_jpeg.h"
#include "jpg/jpeg_to_image.h"

class QibaoLcdDisplay : public RgbLcdDisplay {
public:
    // 物理屏尺寸（横屏）
    static constexpr int kScreenW = 800;
    static constexpr int kScreenH = 480;
    // 竖屏布局尺寸
    static constexpr int kPortraitW = 480;
    static constexpr int kPortraitH = 800;

    QibaoLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y,
                    bool mirror_x, bool mirror_y, bool swap_xy)
        : RgbLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {
        // 方向一变（横屏 <-> 竖屏）就重建界面
        qibao::SetRotationChangedCallback(OnRotationChanged);
    }

    void SetupUI() override {
        // 防止重复建（RebuildUI 之前会把 setup_ui_called_ 清掉）
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetupUI() 重复调用，跳过");
            return;
        }
        s_instance_ = this;

        DisplayLockGuard lock(this);

        EnsureLayoutRoot();                // 按当前方向建/复用布局根容器
        SetLayoutRoot(layout_root_, root_w_, root_h_);

        RgbLcdDisplay::SetupUI();          // 先建好小智原生 UI（状态栏/字幕/表情区）

        ApplyBackdrop();                   // 米灰渐变背景 + 让原生容器透明
        CreatePersonaImages();             // 人物图（解码结果按朝向缓存，不重复解码）
        CreateRotateButton();              // 左下角旋转按钮
        CreateCameraUi();                  // 右上角摄像头按钮 + 小窗（关着就只有按钮）
        CreateTalkLayer();                 // 备用：物理屏上的透明层
        qibao::SetPhysicalTapCallback([](int x, int y) {
            if (QibaoLcdDisplay* self = QibaoLcdDisplay::Instance()) {
                self->HandlePhysicalTap(x, y);
            }
        });

        // 【关键】界面建完了，现在才能安全地套上旋转变换。
        // 原因见 qibao_screen.h 的说明 6：界面没建就设 transform，
        // 后面各个控件的小范围刷新会按「没翻转」的坐标把内容画回去。
        ApplyRootTransform();
        qibao::ScheduleFullRedraw();       // DIRECT 双缓冲：连补几次整屏重画
    }

    // ---------------------------------------------------------------------------
    // 方向变化处理（在 LVGL 线程里被调用）
    // ---------------------------------------------------------------------------
    static void OnRotationChanged(qibao::Rotation rotation) {
        auto* self = s_instance_;
        if (self == nullptr) {
            return;
        }
        self->ApplyRotation(rotation);
    }

    void ApplyRotation(qibao::Rotation rotation) {
        DisplayLockGuard lock(this);
        const bool portrait = (rotation == qibao::Rotation::kPortrait ||
                               rotation == qibao::Rotation::kPortraitAlt);
        const int nw = portrait ? kPortraitW : kScreenW;
        const int nh = portrait ? kPortraitH : kScreenH;

        if (layout_root_ != nullptr && nw == root_w_ && nh == root_h_) {
            // 只是角度变了（0<->180 或 90<->270）：布局尺寸没变，不用重建界面，
            // 改一下 transform 就行。
            ESP_LOGI(TAG, "方向切换（布局不变）：%s", qibao::RotationName().c_str());
            ApplyRootTransform();
            qibao::ScheduleFullRedraw();
            return;
        }

        // 布局尺寸变了（横屏 <-> 竖屏）：整套重建
        ESP_LOGI(TAG, "方向切换（布局 %dx%d -> %dx%d），重建界面", root_w_, root_h_, nw, nh);
        ClearUIObjects();        // 删掉原生 UI 对象
        DestroyLayoutRoot();     // 删掉旧布局根（人物图/按钮一起没了）
        EnsureLayoutRoot();      // 按新尺寸建布局根
        SetLayoutRoot(layout_root_, root_w_, root_h_);
        setup_ui_called_ = false;
        SetupUI();               // 重建（里面会再调 EnsureLayoutRoot，幂等）
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

    // GT1151 直接报上来的按下。不经过会跟着画面旋转的 LVGL 热区。
    // 给语音工具用：摄像头任务已经改好开关之后，把右上角小窗显示出来。
    void SyncCameraChrome() {
        DisplayLockGuard lock(this);
        ApplyCameraMode();
    }

    // 小智自带的 self.camera.take_photo 会把一张半屏静帧铺到画面中间。
    // 这块 SC101IOT 是 UYVY，那张静帧不是右上角实时小窗，用户看到的是一块黑的。
    // 这里直接丢掉，不让它盖住七宝。实时画面由 self.qibao.camera 打开。
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        DisplayLockGuard lock(this);
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_timer_ != nullptr) {
            esp_timer_stop(preview_timer_);
        }
        image.reset();
        preview_image_cached_.reset();
        ESP_LOGI(TAG, "丢掉拍照静帧，不显示半屏黑块");
    }

    void HandlePhysicalTap(int x, int y) {
        static int64_t last_us = 0;
        const int64_t now = esp_timer_get_time();
        if (now - last_us < 600000) {
            return;
        }
        lv_point_t local = {(int32_t)x, (int32_t)y};
        if (layout_root_ != nullptr) {
            lv_obj_transform_point(layout_root_, &local, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
        }
        auto hit = [&](lv_obj_t* obj) {
            if (obj == nullptr || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
            lv_area_t area;
            lv_obj_get_coords(obj, &area);
            return local.x >= area.x1 && local.x <= area.x2 &&
                   local.y >= area.y1 && local.y <= area.y2;
        };
        if (hit(rotate_btn_)) {
            last_us = now;
            ESP_LOGI(TAG, "轻点落在旋转按钮 (%d,%d)", x, y);
            qibao::ToggleRotation();
            return;
        }
        if (hit(cam_btn_)) {
            last_us = now;
            ESP_LOGI(TAG, "轻点摄像头按钮 (%d,%d)", x, y);
            ToggleCamera();
            return;
        }
        if (hit(pip_close_)) {
            last_us = now;
            ESP_LOGI(TAG, "轻点小窗关闭 (%d,%d)", x, y);
            qibao::SetCameraWatch(false);
            ApplyCameraMode();
            return;
        }
        if (hit(pip_box_)) {
            last_us = now;
            ESP_LOGI(TAG, "轻点小窗，对调大小 (%d,%d)", x, y);
            qibao::SetCameraWatchLarge(!qibao::CameraWatchLarge());
            ApplyCameraMode();
            return;
        }
        last_us = now;
        ESP_LOGI(TAG, "轻触屏幕 (%d,%d)：和七宝开始/结束对话", x, y);
        Application::GetInstance().ToggleChatState();
    }

    // ---------------------------------------------------------------------------
    // 忠实快照：直接读「正在送显的那块缓冲」。
    //
    // 上游 LvglDisplay::SnapshotToJpeg() 用的是 lv_snapshot_take(screen, ...)，
    // 而 LVGL 9.5 的整屏快照走的是 lv_obj_redraw()（见 lv_snapshot.c）：
    //     if(top_obj == obj) lv_obj_redraw(...);   // 整屏快照，变换被跳过
    //     else              lv_obj_refr(...);     // 快照子对象才走变换
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

    // 一套人物图（两张：站姿 + 坐姿），按朝向各缓存一份
    struct FrameSet {
        lv_image_dsc_t dsc[2] = {};
        bool ok[2] = {false, false};
        bool decoded = false;
    };
    FrameSet frames_[2];  // [0]=横屏(480 高)  [1]=竖屏(800 高)

    lv_obj_t* layout_root_ = nullptr;  // 布局根容器（整套界面挂在它下面）
    lv_obj_t* talk_hit_ = nullptr;     // 贴在物理屏上的轻点层，不跟布局一起转
    int root_w_ = 0;
    int root_h_ = 0;
    lv_obj_t* imgs_[2] = {nullptr, nullptr};
    lv_obj_t* rotate_btn_ = nullptr;
    lv_obj_t* cam_btn_ = nullptr;
    lv_obj_t* pip_box_ = nullptr;
    lv_obj_t* pip_img_ = nullptr;
    lv_obj_t* pip_label_ = nullptr;
    lv_obj_t* pip_close_ = nullptr;
    lv_obj_t* cam_full_ = nullptr;
    lv_timer_t* cam_timer_ = nullptr;
    uint8_t* pip_px_ = nullptr;
    uint8_t* full_px_ = nullptr;
    lv_image_dsc_t pip_dsc_{};
    lv_image_dsc_t full_dsc_{};
    int pip_w_ = 0;
    int pip_h_ = 0;
    int cur_frame_ = 0;

    // ---------------------------------------------------------------------------
    // 布局根容器
    // ---------------------------------------------------------------------------
    void EnsureLayoutRoot() {
        const bool portrait = qibao::IsPortrait();
        const int nw = portrait ? kPortraitW : kScreenW;
        const int nh = portrait ? kPortraitH : kScreenH;

        if (layout_root_ != nullptr && nw == root_w_ && nh == root_h_) {
            return;  // 尺寸没变，复用
        }

        DestroyLayoutRoot();

        layout_root_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(layout_root_, nw, nh);
        // 居中：横屏时正好铺满屏幕；竖屏时上下各超出 160px，
        // 绕中心转 90° 之后正好变成 800x480 铺满物理屏
        lv_obj_align(layout_root_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_width(layout_root_, 0, 0);
        lv_obj_set_style_pad_all(layout_root_, 0, 0);
        lv_obj_set_style_radius(layout_root_, 0, 0);
        lv_obj_remove_flag(layout_root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(layout_root_, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
        lv_obj_clear_flag(layout_root_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);

        root_w_ = nw;
        root_h_ = nh;
        qibao::SetLayoutRootHandle(layout_root_);
        ESP_LOGI(TAG, "布局根容器: %dx%d（%s）", nw, nh, portrait ? "竖屏" : "横屏");
    }

    void DestroyLayoutRoot() {
        if (layout_root_ == nullptr) {
            return;
        }
        // 根容器被删掉，里面的人物图/按钮也跟着没了，指针必须置空
        imgs_[0] = nullptr;
        imgs_[1] = nullptr;
        rotate_btn_ = nullptr;
        cam_btn_ = nullptr;
        pip_box_ = nullptr;
        pip_img_ = nullptr;
        pip_label_ = nullptr;
        pip_close_ = nullptr;
        cam_full_ = nullptr;
        if (cam_timer_ != nullptr) {
            lv_timer_delete(cam_timer_);
            cam_timer_ = nullptr;
        }
        heap_caps_free(pip_px_);
        pip_px_ = nullptr;
        heap_caps_free(full_px_);
        full_px_ = nullptr;
        if (talk_hit_ != nullptr) {
            lv_obj_del(talk_hit_);
            talk_hit_ = nullptr;
        }
        lv_obj_del(layout_root_);
        layout_root_ = nullptr;
        qibao::SetLayoutRootHandle(nullptr);
        root_w_ = 0;
        root_h_ = 0;
    }

    void ApplyRootTransform() {
        if (layout_root_ == nullptr) {
            return;
        }
        // 【关键】显式把变换中心设到容器正中（50%/50%）。
        // LVGL 的 transform_pivot 默认值是 0，也就是对象左上角；
        // 不设的话画面会绕左上角转，直接飞出屏幕。
        lv_obj_set_style_transform_pivot_x(layout_root_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(layout_root_, LV_PCT(50), 0);
        // 角度单位是 0.1 度：900 = 90°，2700 = 270°
        const int32_t angle = (int32_t)qibao::CurrentRotation();
        lv_obj_set_style_transform_rotation(layout_root_, angle, 0);
        lv_obj_invalidate(lv_screen_active());
        ESP_LOGI(TAG, "应用屏幕变换: rotation=%d (%.0f°)", angle, angle / 10.0);
    }

    // ---------------------------------------------------------------------------
    // 背景与原生 UI 的适配
    // ---------------------------------------------------------------------------
    void ApplyBackdrop() {
        if (layout_root_ == nullptr) {
            return;
        }
        // 屏幕背景设成与原图一致的米灰渐变（顶 #81796D -> 底 #CBC3B7），
        // 这样全身图两侧的留白和图片背景无缝融合，看起来像一整张铺满的图。
        lv_obj_set_style_bg_color(layout_root_, lv_color_hex(0x81796D), 0);
        lv_obj_set_style_bg_grad_color(layout_root_, lv_color_hex(0xCBC3B7), 0);
        lv_obj_set_style_bg_grad_dir(layout_root_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(layout_root_, LV_OPA_COVER, 0);

        // 小智原生的 container_ 是整屏不透明白底，会把上面的渐变盖掉，
        // 两侧就变成两条白边。这里把它改成透明，渐变露出来。
        if (container_ != nullptr) {
            lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(container_, 0, 0);
        }

        // 竖屏时人物图铺满整个画面，聊天文字压在人物身上会看不清，
        // 所以把底部字幕条改成不透明（横屏保持半透明，不影响原来的观感）。
        if (bottom_bar_ != nullptr && qibao::IsPortrait()) {
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0x2A2A2A), 0);
            lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
        }
    }

    // ---------------------------------------------------------------------------
    // 人物图
    // ---------------------------------------------------------------------------
    void CreatePersonaImages() {
        if (layout_root_ == nullptr) {
            return;
        }
        const bool portrait = qibao::IsPortrait();
        if (!EnsureFrames(portrait)) {
            ESP_LOGE(TAG, "人物图解码失败，跳过叠加");
            return;
        }
        FrameSet& fs = frames_[portrait ? 1 : 0];
        const lv_image_dsc_t* first = fs.ok[0] ? &fs.dsc[0] : (fs.ok[1] ? &fs.dsc[1] : nullptr);
        // 图1 也解码成功时用它做第二帧，否则退化成单张静态图
        const lv_image_dsc_t* second = (fs.ok[0] && fs.ok[1]) ? &fs.dsc[1] : nullptr;

        for (int i = 0; i < 2; ++i) {
            const lv_image_dsc_t* dsc = (i == 0) ? first : second;
            if (dsc == nullptr) break;
            lv_obj_t* img = lv_img_create(layout_root_);
            lv_image_set_src(img, dsc);
            lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_TRANSP, 0);
            // 人物图本身不吃触摸。轻点交给上面的透明层，用来开口说话。
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
            imgs_[i] = img;
        }
        if (imgs_[0] != nullptr) lv_obj_move_foreground(imgs_[0]);
        if (imgs_[1] != nullptr) lv_obj_move_foreground(imgs_[1]);

        cur_frame_ = 0;
        if (imgs_[1] != nullptr) {
            lv_obj_set_style_opa(imgs_[1], LV_OPA_TRANSP, 0);
            ESP_LOGI(TAG, "七宝完整全身图 x2 已显示（轻点画面说话，左下角按钮转 90°）");
        } else {
            ESP_LOGI(TAG, "七宝完整全身图 x1 已显示（静态）");
        }
    }

    // 按朝向解码人物图；解过一次就缓存住，不重复解码
    bool EnsureFrames(bool portrait) {
        FrameSet& fs = frames_[portrait ? 1 : 0];
        if (fs.decoded) {
            return fs.ok[0] || fs.ok[1];
        }
        fs.decoded = true;
        const int fit_w = portrait ? kPortraitW : kScreenW;
        const int fit_h = portrait ? kPortraitH : kScreenH;
        const int64_t t0 = esp_timer_get_time();
        fs.ok[0] = DecodeFrame(qibao_frame_0_jpg, sizeof(qibao_frame_0_jpg), &fs.dsc[0], 0, fit_w, fit_h);
        fs.ok[1] = DecodeFrame(qibao_frame_1_jpg, sizeof(qibao_frame_1_jpg), &fs.dsc[1], 1, fit_w, fit_h);
        ESP_LOGI(TAG, "人物图解码(%s) 耗时 %lld ms", portrait ? "竖屏" : "横屏",
                 (long long)((esp_timer_get_time() - t0) / 1000));
        return fs.ok[0] || fs.ok[1];
    }

    // 手指点在旋转按钮的可见位置上吗？
    // 按钮画在已经旋转的布局里，触摸点是物理屏坐标，要先按布局的逆变换换算。
    bool HitsRotateButton(lv_point_t screen_pt) const {
        if (rotate_btn_ == nullptr || layout_root_ == nullptr) {
            return false;
        }
        lv_obj_transform_point(layout_root_, &screen_pt, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
        lv_area_t area;
        lv_obj_get_coords(rotate_btn_, &area);
        lv_area_increase(&area, 16, 16);
        return screen_pt.x >= area.x1 && screen_pt.x <= area.x2 &&
               screen_pt.y >= area.y1 && screen_pt.y <= area.y2;
    }

    // 轻点物理屏 → 和七宝开始或结束这一轮对话（等同短按 BOOT/MODE、串口 qtalk）。
    // 用 PRESSED 而不是 CLICKED：点一下就开口，不要求按下和松开还落在同一个旋转后的控件上。
    static void OnScreenPressed(lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
            return;
        }
        auto* self = static_cast<QibaoLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        lv_point_t p = {0, 0};
        lv_indev_t* indev = lv_indev_active();
        if (indev != nullptr) {
            lv_indev_get_point(indev, &p);
        }
        self->HandlePhysicalTap(p.x, p.y);
    }

    // 轻点层贴在物理屏（800x480）上，不放进会旋转的布局根。
    // 上一版把透明层放进布局根：竖屏时布局是 480x800 再转 90°，
    // 触摸仍是物理屏坐标，热区和手指对不上，CLICKED 根本不会进 ToggleChatState。
    // 串口 qtalk 不经过这块热区，所以只有它能进 listening。
    void CreateTalkLayer() {
        if (talk_hit_ != nullptr) {
            lv_obj_del(talk_hit_);
            talk_hit_ = nullptr;
        }
        talk_hit_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(talk_hit_, kScreenW, kScreenH);
        lv_obj_set_pos(talk_hit_, 0, 0);
        lv_obj_set_style_bg_opa(talk_hit_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(talk_hit_, 0, 0);
        lv_obj_set_style_pad_all(talk_hit_, 0, 0);
        lv_obj_set_style_radius(talk_hit_, 0, 0);
        lv_obj_remove_flag(talk_hit_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(talk_hit_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(talk_hit_, LV_OBJ_FLAG_FLOATING);
        lv_obj_add_event_cb(talk_hit_, OnScreenPressed, LV_EVENT_PRESSED, this);
        lv_obj_move_foreground(talk_hit_);
    }

    void MakeRgbDsc(lv_image_dsc_t* dsc, uint8_t* px, int w, int h) {
        dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf = LV_COLOR_FORMAT_RGB565;
        dsc->header.w = (uint32_t)w;
        dsc->header.h = (uint32_t)h;
        dsc->header.stride = (uint32_t)w * 2;
        dsc->data_size = (uint32_t)w * h * 2;
        dsc->data = px;
    }

    void ToggleCamera() {
        if (qibao::CameraWatchOn()) {
            qibao::SetCameraWatch(false);
        } else {
            qibao::SetCameraWatch(true);
        }
        ApplyCameraMode();
    }

    void ApplyCameraMode() {
        const bool on = qibao::CameraWatchOn();
        const bool large = on && qibao::CameraWatchLarge();
        if (cam_btn_ != nullptr) {
            lv_obj_set_style_bg_color(cam_btn_,
                                      lv_color_hex(on ? 0xC4554A : 0x3A3A3A), 0);
        }
        if (pip_box_ != nullptr) {
            if (on) lv_obj_remove_flag(pip_box_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(pip_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (cam_full_ != nullptr) {
            if (large) lv_obj_remove_flag(cam_full_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(cam_full_, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t* img : imgs_) {
            if (img == nullptr) continue;
            if (large) lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
        }
        if (pip_img_ != nullptr && pip_box_ != nullptr) {
            if (large && imgs_[cur_frame_] != nullptr) {
                lv_image_set_src(pip_img_, lv_image_get_src(imgs_[cur_frame_]));
            } else if (pip_px_ != nullptr) {
                lv_image_set_src(pip_img_, &pip_dsc_);
            }
        }
        if (on && cam_timer_ == nullptr) {
            cam_timer_ = lv_timer_create([](lv_timer_t* timer) {
                auto* self = static_cast<QibaoLcdDisplay*>(lv_timer_get_user_data(timer));
                if (self != nullptr) self->RefreshCameraFrame();
            }, 200, this);
        }
        if (!on && cam_timer_ != nullptr) {
            lv_timer_delete(cam_timer_);
            cam_timer_ = nullptr;
        }
        if (pip_label_ != nullptr) {
            const char* badge = qibao::ViewerBadge();
            lv_label_set_text(pip_label_, badge != nullptr ? badge : "");
        }
    }

    void RefreshCameraFrame() {
        if (!qibao::CameraWatchOn()) return;
        if (qibao::CameraWatchLarge()) {
            if (full_px_ != nullptr &&
                qibao::CopyLatestFrame(full_px_, root_w_, root_h_) && cam_full_ != nullptr) {
                lv_obj_invalidate(cam_full_);
            }
        } else if (pip_px_ != nullptr &&
                   qibao::CopyLatestFrame(pip_px_, pip_w_, pip_h_) && pip_img_ != nullptr) {
            lv_image_set_src(pip_img_, &pip_dsc_);
            lv_obj_invalidate(pip_img_);
        }
        if (pip_label_ != nullptr) {
            const char* badge = qibao::ViewerBadge();
            lv_label_set_text(pip_label_, badge != nullptr ? badge : "");
        }
    }

    void CreateCameraUi() {
        if (layout_root_ == nullptr) return;
        pip_w_ = root_w_ > root_h_ ? 200 : 168;
        pip_h_ = pip_w_ * 3 / 4;
        const int btn = 56;
        const int bx = root_w_ - 12 - btn;
        const int by = 42;
        const int px = root_w_ - 12 - pip_w_;
        const int py = by + btn + 8;

        cam_btn_ = lv_btn_create(layout_root_);
        lv_obj_set_size(cam_btn_, btn, btn);
        lv_obj_set_pos(cam_btn_, bx, by);
        lv_obj_set_style_radius(cam_btn_, btn / 2, 0);
        lv_obj_set_style_bg_color(cam_btn_, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_border_width(cam_btn_, 2, 0);
        lv_obj_set_style_border_color(cam_btn_, lv_color_hex(0xF4EFE6), 0);
        lv_obj_t* lens = lv_obj_create(cam_btn_);
        lv_obj_set_size(lens, 22, 22);
        lv_obj_center(lens);
        lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lens, lv_color_hex(0xF4EFE6), 0);
        lv_obj_set_style_border_width(lens, 0, 0);
        lv_obj_remove_flag(lens, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* hole = lv_obj_create(lens);
        lv_obj_set_size(hole, 10, 10);
        lv_obj_center(hole);
        lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(hole, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_border_width(hole, 0, 0);
        lv_obj_remove_flag(hole, LV_OBJ_FLAG_CLICKABLE);

        pip_box_ = lv_obj_create(layout_root_);
        lv_obj_set_size(pip_box_, pip_w_, pip_h_);
        lv_obj_set_pos(pip_box_, px, py);
        lv_obj_set_style_radius(pip_box_, 12, 0);
        lv_obj_set_style_bg_color(pip_box_, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_border_width(pip_box_, 3, 0);
        lv_obj_set_style_border_color(pip_box_, lv_color_hex(0xF4EFE6), 0);
        lv_obj_set_style_pad_all(pip_box_, 0, 0);
        lv_obj_set_style_clip_corner(pip_box_, true, 0);
        lv_obj_remove_flag(pip_box_, LV_OBJ_FLAG_SCROLLABLE);

        pip_px_ = (uint8_t*)heap_caps_malloc((size_t)pip_w_ * pip_h_ * 2,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pip_px_ != nullptr) {
            std::memset(pip_px_, 0, (size_t)pip_w_ * pip_h_ * 2);
            MakeRgbDsc(&pip_dsc_, pip_px_, pip_w_, pip_h_);
        }
        full_px_ = (uint8_t*)heap_caps_malloc((size_t)root_w_ * root_h_ * 2,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (full_px_ != nullptr) {
            std::memset(full_px_, 0, (size_t)root_w_ * root_h_ * 2);
            MakeRgbDsc(&full_dsc_, full_px_, root_w_, root_h_);
        }

        pip_img_ = lv_image_create(pip_box_);
        if (pip_px_ != nullptr) lv_image_set_src(pip_img_, &pip_dsc_);
        lv_obj_center(pip_img_);
        lv_obj_remove_flag(pip_img_, LV_OBJ_FLAG_CLICKABLE);

        pip_label_ = lv_label_create(pip_box_);
        lv_label_set_text(pip_label_, "");
        lv_obj_align(pip_label_, LV_ALIGN_BOTTOM_LEFT, 6, -4);
        lv_obj_set_style_text_color(pip_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_remove_flag(pip_label_, LV_OBJ_FLAG_CLICKABLE);

        pip_close_ = lv_btn_create(pip_box_);
        lv_obj_set_size(pip_close_, 28, 28);
        lv_obj_align(pip_close_, LV_ALIGN_TOP_RIGHT, -4, 4);
        lv_obj_set_style_radius(pip_close_, 14, 0);
        lv_obj_set_style_bg_color(pip_close_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(pip_close_, LV_OPA_70, 0);
        lv_obj_t* xlbl = lv_label_create(pip_close_);
        lv_label_set_text(xlbl, "x");
        lv_obj_center(xlbl);
        lv_obj_set_style_text_color(xlbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_remove_flag(xlbl, LV_OBJ_FLAG_CLICKABLE);

        cam_full_ = lv_image_create(layout_root_);
        if (full_px_ != nullptr) lv_image_set_src(cam_full_, &full_dsc_);
        lv_obj_set_pos(cam_full_, 0, 0);
        lv_obj_add_flag(cam_full_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cam_full_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(cam_full_);

        lv_obj_add_flag(pip_box_, LV_OBJ_FLAG_HIDDEN);
        if (qibao::CameraWatchOn()) {
            ApplyCameraMode();
        }
        ESP_LOGI(TAG, "摄像头按钮在右上角，小窗 %dx%d", pip_w_, pip_h_);
    }

    // 点旋转按钮 → 整个画面转 90°
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
    void CreateRotateButton() {
        if (layout_root_ == nullptr) {
            return;
        }
        lv_obj_t* btn = lv_btn_create(layout_root_);
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
        rotate_btn_ = btn;

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

    // 解码到 PSRAM，常驻不释放（形象图一直显示）。
    // 【注意】LVGL 删除图片对象时不会释放 VARIABLE 类型的图源，
    // 所以这里解码出来的数据可以在界面重建时继续复用，不用重新解码。
    static bool DecodeFrame(const unsigned char* jpg, size_t len, lv_image_dsc_t* dsc, int idx,
                            int fit_w, int fit_h) {
        uint8_t* out = nullptr;
        size_t out_len = 0, w = 0, h = 0, stride = 0;
        // max 传目标框尺寸，解码器会按比例缩放并 round 到 8 的倍数；
        // 1080x1920 -> 横屏约 270x480，竖屏约 450x800。
        esp_err_t err = jpeg_to_image_fit(jpg, len, fit_w, fit_h, &out, &out_len, &w, &h, &stride);
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
};
