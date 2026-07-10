/**
 * @file    device_led.h
 * @brief   LED 字幕屏控制模块（串口/网口）
 *
 *          V2.6:
 *          - cmd=201: 字幕播放 (textData/lightVal/showType/switchTime/displayStyle)
 *          - cmd=202: 开关控制 (switch)
 */

#ifndef DEVICE_LED_H
#define DEVICE_LED_H

#include "cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

int   device_led_init(const char *uart_dev, int baudrate,
                      const char *net_ip, int net_port);
int   device_led_execute(const cmd_t *cmd, cJSON **resp);
cJSON *device_led_get_status(void);
int   device_led_get_fail_status(void);
void  device_led_apply_config(const char *text, int light_val,
                              int show_type, int display_style);
void  device_led_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_LED_H */
