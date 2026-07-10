/**
 * @file    device_auth.c
 * @brief   设备认证实现（cmd=11）
 *
 *          WebSocket 连接后首次交互，控制板发送设备信息获取 token。
 *          此处处理服务器的认证回复（含 token）。
 */

#include "device_auth.h"

#include <stdio.h>
#include <string.h>

#include "msg_builder.h"
#include "config.h"

/** 全局 token（认证成功后存储，后续消息使用） */
static char g_auth_token[256] = {0};

int device_auth_execute(const cmd_t *cmd, cJSON **resp)
{
    (void)cmd;

    printf("[AUTH] 收到认证回复\n");

    /* 从服务器回复中提取 token */
    cJSON *token_item = cJSON_GetObjectItem(cmd->root, "token");
    if (cJSON_IsString(token_item)) {
        strncpy(g_auth_token, cJSON_GetStringValue(token_item),
                sizeof(g_auth_token) - 1);
        printf("[AUTH] token 已保存\n");
    }

    /* 提取服务器时间用于校时 */
    const char *server_time = cmd_get_string(cmd, "serverTimes");
    if (server_time) {
        printf("[AUTH] 服务器时间: %s\n", server_time);
        /* TODO: 调用 settimeofday() 同步系统时间 */
    }

    if (resp) {
        *resp = msg_build_response(ERR_SUCCESS, "auth ok");
    }
    return 0;
}

const char *device_auth_get_token(void)
{
    return g_auth_token[0] ? g_auth_token : NULL;
}
