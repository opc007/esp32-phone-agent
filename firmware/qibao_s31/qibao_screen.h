/*
 * qibao_screen.h —— 七宝的屏幕方向管理（手动翻转）
 *
 * 这块板没有加速度计，自动跟随重力做不到；改成手动：
 *   - 板载 MODE 键长按 2 秒 → 切到下一个方向
 *   - 屏幕左下角的旋转按钮 → 切到下一个方向
 *   - 串口 qrotate → 切到下一个方向
 *
 * ---------------------------------------------------------------------------
 * 实现原理（踩过坑才定下来的，别随便改回去）
 *
 * 1) 为什么不用面板级 mirror / swap：
 *    RGB 面板 + LVGL direct 模式，内容直接画进面板自己的帧缓冲，
 *    面板的 mirror/swap 只在「CPU 拷贝到帧缓冲」那条路上才处理，
 *    direct 模式下完全无效。
 *
 * 2) 为什么不用 lv_display_set_rotation + 矩阵：
 *    LVGL 9.5 的矩阵旋转只在 LV_DRAW_TRANSFORM_USE_MATRIX 打开时生效，
 *    而那条路会把变换写进 layer->matrix -> task->matrix，
 *    纯软件渲染器（LV_DRAW_SW）根本不消费 task->matrix —— 画面纹丝不动。
 *    实测探针：旋转前后送显缓冲校验和完全一样（03fa93f3 -> 03fa93f3）。
 *
 * 3) 最终方案：对象级变换 lv_obj_set_style_transform_rotation(screen, ...)
 *    LVGL 经典路径：lv_obj_refr() 发现 rotation != 0 ->
 *    calculate_layer_type() 判定 LV_LAYER_TYPE_TRANSFORM ->
 *    先把整个对象画进一张独立图层，再由 lv_draw_sw_transform() 把图层
 *    带角度贴回主图层。这条路纯软件渲染器是支持的（RGB565 也在支持列表里），
 *    内容在送显缓冲里就已经是转好的，零额外 CPU 开销。
 *
 * 4) 【关键】transform_pivot 必须显式设成屏幕中心：
 *    LVGL 的 transform_pivot 默认值是 0（对象左上角），不是 50%。
 *    不设的话 180° 会绕左上角转，整个画面直接飞出屏幕 —— 表现就是
 *    「按了旋转但画面转不过来」。所以这里显式写 LV_PCT(50)。
 *
 * 5) 【关键】不要再用 esp_lcd_touch_set_mirror_x/y 做触摸镜像：
 *    LVGL 命中判定自带逆变换 —— lv_indev_search_obj() 会调
 *    lv_obj_transform_point(..., LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE)，
 *    把触摸点反变换回对象坐标。驱动层再镜像一次就成了双重翻转，
 *    点了左边却触发右边。交给 LVGL 自己处理，保证「点哪就是哪」。
 */
#ifndef QIBAO_SCREEN_H
#define QIBAO_SCREEN_H

#include <esp_lcd_touch.h>
#include <lvgl.h>

#include <string>

namespace qibao {

// 屏幕方向：按一次切到下一个方向（0° -> 180° -> 0° ...）
// 90°/270° 不做：内容会超出 800x480 被裁掉，不如给两个绝对正确的方向。
enum class Rotation {
    kNormal = 0,   // 0°  正向
    kUpsideDown,   // 180° 倒置
};

// 绑定显示控制器，并恢复上次保存的方向。必须在显示初始化完之后调用。
void InitScreen(esp_lcd_touch_handle_t touch);

// 应用上次保存的方向（0° / 180°）。
//
// 【必须在 UI 建好之后调用】
// LVGL 的失效区域计算（lv_obj_pos.c 的 invalidate_area_core）**不会**把父级的
// transform 算进去：屏幕内容还没建的时候设 transform，LVGL 只会把一张空屏转一遍；
// 之后各个控件创建时的小范围刷新，又会按「没翻转」的坐标把内容画回去 ——
// 结果开机画面是正立的（实测：开机时明明 rotation=1800，画面却没翻）。
// 所以方向要在 SetupUI() 建完界面之后再应用。
void ApplySavedRotation();

// 切到下一个方向，返回切换后的中文描述
std::string ToggleRotation();

// 直接指定方向，返回是否设置成功
bool SetRotation(Rotation rotation);

// 当前方向
Rotation CurrentRotation();

// 当前是否为 180° 倒置
bool IsRotated();

// 当前方向的中文描述，例如「180° 倒置」
std::string RotationName();

// 调试用：读当前送显缓冲，打印正序/旋转序两个采样校验和。
// 画面翻转 180° 后，这两个值应该互换 —— 证明旋转真的作用到了送显内容。
void ProbeFrame();

// 调试用：打印 LVGL 内部与变换相关的状态（样式、图层类型、子对象）。
// 可从任意线程调用，内部会切到 LVGL 线程执行。
void DumpTransformState();

// 调试用：在屏幕左上角放洋红标记，一帧后读送显缓冲的四角像素。
// 标记出现在右下角 = 180° 变换真的作用到了画面；还在左上角 = 没生效。
// 这是比整屏校验和可靠得多的判据（激活页文字一直在变，会污染校验和）。
void RunMarkerTest();

}  // namespace qibao

#endif  // QIBAO_SCREEN_H
