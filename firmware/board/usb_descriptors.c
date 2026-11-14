/*
 * USB HID 复合设备描述符 —— 键盘 + 鼠标 + 触摸屏(digitizer) + 多媒体键
 *
 * 单个 HID 接口，通过 Report ID 区分四种 report：
 *   1 = 键盘 (boot keyboard)
 *   2 = 鼠标 (相对坐标)
 *   3 = 触摸屏 (绝对坐标，Android 识别为外接触摸屏，可精确点击/滑动)
 *   4 = 多媒体/消费者控制键 (音量等)
 */

#include "tusb.h"
#include "tinyusb.h"
#include "sdkconfig.h"

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_TOTAL,
};

#define EPNUM_HID 0x81

#define PHONE_HID_EP_SIZE 16

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

/* ------------------------------------------------------------------ */
/* Report descriptor                                                    */
/* ------------------------------------------------------------------ */

/* 单点触摸 digitizer（绝对坐标 0..4095，Tip Switch）
 * Report: [id][x_lo][x_hi][y_lo][y_hi][tip] 共 6 字节 */
#define TUD_HID_REPORT_DESC_TOUCHSCREEN(report_id) \
    0x05, 0x0D,          /* Usage Page (Digitizers)              */ \
    0x09, 0x04,          /* Usage (Touchscreen)                  */ \
    0xA1, 0x01,          /* Collection (Application)             */ \
    HID_REPORT_ID(report_id)                                             \
    0x09, 0x22,          /*   Usage (Finger)                     */ \
    0xA1, 0x00,          /*   Collection (Physical)              */ \
    0x09, 0x42,          /*     Usage (Tip Switch)               */ \
    0x15, 0x00,          /*     Logical Minimum (0)              */ \
    0x25, 0x01,          /*     Logical Maximum (1)              */ \
    0x75, 0x01,          /*     Report Size (1)                  */ \
    0x95, 0x01,          /*     Report Count (1)                 */ \
    0x81, 0x02,          /*     Input (Data, Var, Abs)           */ \
    0x75, 0x01,          /*     Report Size (1)                  */ \
    0x95, 0x07,          /*     Report Count (7)                 */ \
    0x81, 0x03,          /*     Input (Const, Var, Abs) padding  */ \
    0x05, 0x01,          /*     Usage Page (Generic Desktop)     */ \
    0x09, 0x30,          /*     Usage (X)                        */ \
    0x15, 0x00,          /*     Logical Minimum (0)              */ \
    0x26, 0xFF, 0x0F,    /*     Logical Maximum (4095)           */ \
    0x75, 0x10,          /*     Report Size (16)                 */ \
    0x95, 0x01,          /*     Report Count (1)                 */ \
    0x81, 0x02,          /*     Input (Data, Var, Abs)           */ \
    0x09, 0x31,          /*     Usage (Y)                        */ \
    0x81, 0x02,          /*     Input (Data, Var, Abs)           */ \
    0xC0,                /*   End Collection (Physical)          */ \
    0xC0                 /* End Collection (Application)         */

uint8_t const hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(2)),
    TUD_HID_REPORT_DESC_TOUCHSCREEN(3),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(4)),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_descriptor;
}

/* ------------------------------------------------------------------ */
/* Configuration descriptor                                             */
/* ------------------------------------------------------------------ */

static const uint8_t configuration_descriptor[] = {
    // config num, itf count, string idx, total len, attr, power mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 200),
    // itf num, string idx, protocol, report desc len, ep in, size, interval
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_descriptor), EPNUM_HID,
                       PHONE_HID_EP_SIZE, 1),
};

const char *string_descriptor[] = {
    (char[]){0x09, 0x04},              // 0: 语言 0x0409
    "XiaoZhi",                          // 1: Manufacturer
    "XiaoZhi Phone Controller",         // 2: Product
    "XZ-PHID-0001",                     // 3: Serial
    "XiaoZhi HID",                      // 4: HID Interface
};

tinyusb_config_t phone_hid_tusb_cfg = {
    .device_descriptor = NULL,
    .string_descriptor = string_descriptor,
    .string_descriptor_count = sizeof(string_descriptor) / sizeof(string_descriptor[0]),
    .external_phy = false,
    .configuration_descriptor = configuration_descriptor,
};

/* ------------------------------------------------------------------ */
/* TinyUSB HID 回调                                                     */
/* ------------------------------------------------------------------ */

__attribute__((weak))
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

__attribute__((weak))
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
