/**
 * @file    device_alarm.h
 * @brief   警灯控制模块（V2.9 — 平台 GPIO 适配）
 *
 *          cmd=401: 警灯控制
 *            - state:      0=关闭, 1=打开
 *            - lightType:  灯光模式 0~14（共15种），默认0
 *            - openType:   0=正常开启, 1=定时开关
 *            - openData:   定时时间（YYYY-MM-DD HH:mm:ss），openType=1时有效
 *
 *          硬件连接:
 *            J_I2C 座子 2脚 → gpio_out4（模式切换）
 *            J_I2C 座子 3脚 → gpio_out3（电源开关，高=开，低=关，默认开）
 *          GPIO 路径:
 *            /sys/devices/platform/gpio_out/gpio_out4/value（模式）
 *            /sys/devices/platform/gpio_out/gpio_out3/value（电源）
 *          模式切换: 高 0.5s + 低 0.5s = 一次，从模式A切到B需 |B-A| 次
 */

#ifndef DEVICE_ALARM_H
#define DEVICE_ALARM_H

#include "cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态变更回调函数类型（可选，当前使用标志位轮询） */
typedef void (*alarm_status_cb_t)(void);

/**
 * @brief   初始化警灯模块
 * @param   gpio_chip   GPIO 芯片名称（保留兼容，当前未使用）
 * @param   gpio_pin    GPIO 引脚号（保留兼容，当前未使用）
 * @return  0 成功
 */
int device_alarm_init(const char *gpio_chip, int gpio_pin);

/**
 * @brief   注册状态变更回调（可选）
 * @param   cb  回调函数指针，传 NULL 则取消注册
 */
void device_alarm_set_status_cb(alarm_status_cb_t cb);

/**
 * @brief   查询警灯状态是否发生变更
 *          调用后自动清除标志位。主循环每轮调用，返回 1 时应立即推送状态。
 * @return  1=有变更需要上报，0=无变更
 */
int device_alarm_poll_status_changed(void);

/**
 * @brief   执行警灯命令
 * @param   cmd     已解析的命令
 * @param   resp    输出，回复 JSON（调用者负责 cJSON_Delete）
 * @return  0 成功，非 0 失败
 */
int device_alarm_execute(const cmd_t *cmd, cJSON **resp);

/**
 * @brief   获取当前警灯状态
 * @return  cJSON 对象（调用者负责 cJSON_Delete）
 */
cJSON *device_alarm_get_status(void);
int    device_alarm_get_fail_status(void);

/**
 * @brief   释放警灯模块资源
 */
void device_alarm_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_ALARM_H */