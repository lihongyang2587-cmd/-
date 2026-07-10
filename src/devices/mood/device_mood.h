/**
 * @file    device_mood.h
 * @brief   氛围灯控制模块
 *
 *          V2.6: 无 cmd 字段，直接传 state/type
 *          - state: "on" | "off"
 *          - type:  "steady" | "breath" | "flash" | "gradient"
 */

#ifndef DEVICE_MOOD_H
#define DEVICE_MOOD_H

#include "cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

int   device_mood_init(void);
int   device_mood_execute(const cmd_t *cmd, cJSON **resp);
cJSON *device_mood_get_status(void);
void  device_mood_apply_config(const char *state, const char *type);
void  device_mood_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_MOOD_H */
