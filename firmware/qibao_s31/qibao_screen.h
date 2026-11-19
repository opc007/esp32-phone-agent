/*
 * qibao_screen.h —— 七宝的屏幕方向管理（手动翻转，四个挡）
 *
 * 这块板没有加速度计，自动跟随重力做不到；改成手动：
 *   - 板载 MODE 键长按 2 秒 → 切到下一个方向
 *   - 屏幕左下角的旋转按钮 → 切到下一个方向
 *   - 串口 qrotate → 切到下一个方向
 *
 * ---------------------------------------------------------------------------
 * 四个挡（每按一次转 90°）
 *
 *   0°    横屏正向    布局 800x480
 *   90°   竖屏        布局 480x800（设备顺时针转 90° 拿）
 *   180°  横屏倒置    布局 800x480
 *   270°  竖屏        布局 480x800（设备逆时针转 90° 拿）
 *
 * 横屏（0°/180°）和竖屏（90°/270°）的**布局尺寸不一样**，
 * 所以换挡时界面要重建，不是简单转个角度。
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
 * 3) 为什么不用「显示级旋转」（lv_display_set_rotation）：
 *    esp_lvgl_port 的触摸代码完全不支持显示级旋转
 *    （esp_lvgl_port_touch.c 只做缩放，不做旋转映射），
 *    旋转后触摸会全部错位，得自己手写一份映射，坑多。
 *
 * 4) 最终方案：**容器级变换**
 *    把整套界面塞进一个「布局根容器」，再对这个容器做
 *    lv_obj_set_style_transform_rotation(...)。理由：
 *      - 竖屏时容器是 480x800，正好绕屏幕中心转 90° 就铺满 800x480 的物理屏；
 *      - LVGL 的命中判定自带逆变换（lv_indev_search_obj 会调
 *        lv_obj_transform_point(..., INVERSE)），触摸自动跟着方向走，
 *        不用手写映射 —— 实测点哪就是哪；
 *      - 局部刷新也正确：lv_refr.c 的 layer_get_area() 会把失效区
 *        逆变换回对象坐标，所以聊天文字更新不会画错地方（已实测）。
 *
 * 5) 【关键】transform_pivot 必须显式设成 50%：
 *    LVGL 的 transform_pivot 默认值是 0（对象左上角），不是 50%。
 *    不设的话画面会绕左上角转，直接飞出屏幕 —— 表现就是
 *    「按了旋转但画面转不过来」。
 *
 * 6) 【关键】变换要在界面建好之后再套上：
 *    界面还没建就设 transform，LVGL 只会把一张空屏转一遍；
 *    之后各个控件创建时的小范围刷新，又会按「没翻转」的坐标把内容画回去 ——
 *    结果开机画面是正立的（实测：开机时明明 rotation=1800，画面却没翻）。
 *
 * 7) 【关键】不要再用 esp_lcd_touch_set_mirror_x/y 做触摸镜像：
 *    LVGL 命中判定自带逆变换，驱动层再镜像一次就成了双重翻转，
 *    点了左边却触发右边。交给 LVGL 自己处理，保证「点哪就是哪」。
 */
#ifndef QIBAO_SCREEN_H
#define QIBAO_SCREEN_H

#include <esp_lcd_touch.h>
#include <lvgl.h>

#include <string>

namespace qibao {

// 屏幕方向：四个挡，每按一次转 90°。
// 枚举值直接就是 LVGL 的「0.1 度」角度，transform_rotation 可以直接拿来用。
//
// 【方向对应关系】（这是最容易搞混的地方）
//   挡位    内容在屏上的旋转   设备该怎么拿
//   0°      不转              横着拿
//   90°     顺时针 90°        设备逆时针转 90° 拿（把横屏板转成竖屏的常规手势）
//   180°    倒转              横着拿但上下颠倒
//   270°    逆时针 90°        设备顺时针转 90° 拿（反方向握）
//
// 为什么 90° 对应「设备逆时针转」：竖屏布局的顶边会被映射到物理屏的**右边**，
// 而设备逆时针转 90° 之后，物理屏的右边正好朝上 —— 顶边就在用户视野的最上方。
enum class Rotation {
    kNormal = 0,        // 0°   横屏正向
    kPortrait = 900,     // 90°  竖屏（内容顺时针转 90°）
    kUpsideDown = 1800,  // 180° 横屏倒置
    kPortraitAlt = 2700  // 270° 竖屏（内容逆时针转 90°）
};

// 绑定显示控制器，并恢复上次保存的方向。必须在显示初始化完之后调用。
void InitScreen(esp_lcd_touch_handle_t touch);

// 当前方向
Rotation CurrentRotation();

// 当前角度（整数度：0 / 90 / 180 / 270）
int CurrentAngleDeg();

// 当前是不是竖屏（90° / 270°）
bool IsPortrait();

// 当前方向的中文描述，例如「90° 竖屏」
std::string RotationName();

// 切到下一个方向（0° -> 90° -> 180° -> 270° -> 0°），返回切换后的中文描述
std::string ToggleRotation();

// 直接指定方向：存 NVS，并让显示层跟着翻（横竖屏切换时界面会重建）
bool SetRotation(Rotation rotation);

// 方向变化通知。显示层注册这个回调，方向一变就重建界面。
using RotationChangedCallback = void (*)(Rotation rotation);
void SetRotationChangedCallback(RotationChangedCallback cb);

// 显示层把「布局根容器」交给方向层：
// 标记测试要往里面放东西，才知道画面转到哪个角去了。
void SetLayoutRootHandle(lv_obj_t* root);

// 换方向后连补几次整屏重画（DIRECT 模式双缓冲的坑，见 .cc 里的说明）
void ScheduleFullRedraw();

// 调试用：读当前送显缓冲，打印正序/旋转序两个采样校验和。
// 画面翻转后，这两个值应该互换 —— 证明旋转真的作用到了送显内容。
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
