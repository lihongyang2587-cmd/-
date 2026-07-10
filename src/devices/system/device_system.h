/**
 * @file    device_system.h
 * @brief   系统管理命令执行器
 */

#ifndef DEVICE_SYSTEM_H
#define DEVICE_SYSTEM_H

#include "cmd_parser.h"   /* ← 直接包含，cmd_t 已在此定义 */

#ifdef __cplusplus
extern "C" {
#endif

int device_system_execute(const cmd_t *cmd, cJSON **resp);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_SYSTEM_H */