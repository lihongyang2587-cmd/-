/**
 * @file    device_auth.h
 * @brief   设备认证模块（cmd=11）
 *
 *          WebSocket 连接建立后，控制板首发认证消息。
 */

#ifndef DEVICE_AUTH_H
#define DEVICE_AUTH_H

#include "cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   执行认证（cmd=11）
 * @param   cmd  已解析的命令
 * @param   resp 输出，回复消息 JSON
 * @return  0 成功
 */
int device_auth_execute(const cmd_t *cmd, cJSON **resp);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_AUTH_H */
