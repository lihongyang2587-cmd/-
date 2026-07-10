/**
 * @file    device_mood.c
 * @brief   氛围灯控制实现（V2.7 — 状态持久化支持）
 *
 *          无 cmd 字段，直接从 root 取 state/type：
 *          {"state": "on", "type": "breath"}
 */

#include "device_mood.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

#include "msg_builder.h"
#include "config.h"

#ifndef ERR_PARAM_INVALID
#define ERR_PARAM_INVALID   1001
#endif
#ifndef ERR_SUCCESS
#define ERR_SUCCESS         0
#endif

/* ======================================================================== */
/*  模块内部状态（V2.7：持久化支持）                                            */
/* ======================================================================== */

static char g_mood_state[32] = "off";
static char g_mood_type[32]  = "steady";

/* 互斥锁，保护状态变量的并发读写 */
static pthread_mutex_t g_mood_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

int device_mood_init(void)
{
    printf("[MOOD] 氛围灯初始化\n");
    return 0;
}

int device_mood_execute(const cmd_t *cmd, cJSON **resp)
{
    const char *state = cmd_get_string(cmd, "state");
    const char *type  = cmd_get_string(cmd, "type");

    printf("[MOOD] state=%s type=%s\n",
           state ? state : "(null)", type ? type : "(null)");

    /* 验证 state 枚举值（仅允许 on / off） */
    if (state && state[0] != '\0') {
        if (strcmp(state, "on") != 0 && strcmp(state, "off") != 0) {
            fprintf(stderr, "[MOOD] state 无效: %s\n", state);
            if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                 "state 无效，必须为 on 或 off");
            return -1;
        }
    }

    /* 验证 type 枚举值（仅允许 steady / breath / flash / gradient） */
    if (type && type[0] != '\0') {
        if (strcmp(type, "steady")   != 0 &&
            strcmp(type, "breath")   != 0 &&
            strcmp(type, "flash")    != 0 &&
            strcmp(type, "gradient") != 0) {
            fprintf(stderr, "[MOOD] type 无效: %s\n", type);
            if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                 "type 无效，必须为 steady/breath/flash/gradient");
            return -1;
        }
    }

    /* 至少需要 state 或 type 之一 */
    if ((!state || state[0] == '\0') && (!type || type[0] == '\0')) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "至少需要 state 或 type 参数");
        return -1;
    }

    /* 更新内存状态（V2.7：持久化支持） */
    if (state || type) {
        pthread_mutex_lock(&g_mood_mutex);

        if (state && state[0] != '\0') {
            snprintf(g_mood_state, sizeof(g_mood_state), "%s", state);
        }
        if (type && type[0] != '\0') {
            snprintf(g_mood_type, sizeof(g_mood_type), "%s", type);
        }

        pthread_mutex_unlock(&g_mood_mutex);
    }

    /*
     * TODO: 实现氛围灯硬件控制
     * if (strcmp(state, "on") == 0) {
     *     // type=steady/breath/flash/gradient → 发送对应指令
     * } else {
     *     // 关闭
     * }
     */

    if (resp) {
        *resp = msg_build_response(ERR_SUCCESS, "success");
    }
    return 0;
}

cJSON *device_mood_get_status(void)
{
    /* V3.1: trylock 防阻塞。mood 执行在主线程，一般不会竞争，但保持与其他模块一致。 */
    char state[32], type[32];

    if (pthread_mutex_trylock(&g_mood_mutex) == 0) {
        snprintf(state, sizeof(state), "%s", g_mood_state);
        snprintf(type,  sizeof(type),  "%s", g_mood_type);
        pthread_mutex_unlock(&g_mood_mutex);
    } else {
        snprintf(state, sizeof(state), "%s", g_mood_state);
        snprintf(type,  sizeof(type),  "%s", g_mood_type);
    }

    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "device", "mood");
    cJSON_AddStringToObject(status, "state",  state);
    cJSON_AddStringToObject(status, "type",   type);
    cJSON_AddBoolToObject  (status, "online", true);

    return status;
}

void device_mood_apply_config(const char *state, const char *type)
{
    pthread_mutex_lock(&g_mood_mutex);

    if (state && state[0] != '\0') {
        snprintf(g_mood_state, sizeof(g_mood_state), "%s", state);
    }
    if (type && type[0] != '\0') {
        snprintf(g_mood_type, sizeof(g_mood_type), "%s", type);
    }

    pthread_mutex_unlock(&g_mood_mutex);

    printf("[MOOD] 配置已恢复: state=%s type=%s\n", g_mood_state, g_mood_type);
}

void device_mood_deinit(void)
{
    printf("[MOOD] 氛围灯已释放\n");
}
