/**
 * @file    msg_builder.h
 * @brief   消息构建器（V2.6 + 补充特殊响应构建）
 *
 *          构建出站消息（Board → Server），采用统一推送格式：
 *          { "event": "...", "timestamp": "...", "data": {...} }
 *
 *          以及 cmd 响应格式：
 *          通用(V2.7): { "code": 0, "cmd": <int>, "msg": "success" }
 *          特殊:    带额外数据的 cmd 响应（601 / 303 等）
 *          遗留:    { "code": 0, "msg": "success" }（无 cmd 字段，待迁移）
 */

#ifndef MSG_BUILDER_H
#define MSG_BUILDER_H

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  出站推送消息（Board → Server，event 格式）                                */
/* ======================================================================== */

cJSON *msg_build_heartbeat(long uptime_sec);
cJSON *msg_build_device_status(void);
cJSON *msg_build_alarm(const char *device_name, const char *message);
cJSON *msg_build_error(int code, const char *message);
cJSON *msg_build_battery(int soc, bool charging);

/* ======================================================================== */
/*  通用 cmd 响应（回复大多数控制指令）                                        */
/*  格式: { "code": 0, "msg": "success" }                                    */
/* ======================================================================== */

cJSON *msg_build_response(int code, const char *msg);

/**
 * @brief   通用 cmd 响应（V2.7 统一格式 — 携带 cmd 字段）
 *          格式: { "code": 0, "cmd": N, "msg": "success" }
 *          适用于所有设备控制指令的回复
 *          (cmd=101~105, 201~202, 301~303, 401, 602, 604~606)
 * @param   code  错误码 (0=成功)
 * @param   cmd   命令ID
 * @param   msg   状态描述
 */
cJSON *msg_build_cmd_response(int code, int cmd, const char *msg);

/* ======================================================================== */
/*  特殊 cmd 响应（回复中需要携带 cmd 字段及专属数据）                          */
/* ======================================================================== */

/**
 * @brief   系统状态采集响应 (cmd=601)
 *          { "cmd": 601, "result": 0 }
 * @param   result  0=成功，1=失败
 */
cJSON *msg_build_sys_status_response(int result);

/**
 * @brief   系统版本查询响应 (cmd=603)
 *          { "cmd": 603, "mainVer": N }
 * @param   mainVer 当前主版本号
 */
cJSON *msg_build_sys_version_response(int mainVer);

/**
 * @brief   音箱音频列表查询响应 (cmd=303)
 *          { "cmd": 303, "result": 0, "msg": "获取成功", "audioNum": N, "audioData": [...] }
 * @param   result      结果码 (0=成功)
 * @param   msg         状态描述
 * @param   audio_num   有效音频文件数量（即使为 0 也会写入）
 * @param   audio_data  cJSON 数组（元素含 index + audioName）；传 NULL 返回空数组
 */
cJSON *msg_build_spk_getlist_response(int result, const char *msg,
                                       int audio_num, cJSON *audio_data);

/* ======================================================================== */
/*  异常状态上报（cmd=607，Board → Server）                                    */
/*  格式: { "cmd": 607, "token": "...", "ptzFailStatus": N, ... }            */
/* ======================================================================== */

cJSON *msg_build_exception_report(int ptz_fail, int led_fail,
                                  int speaker_fail, int alarm_fail);

/* ======================================================================== */
/*  认证消息（Board → Server，cmd=11）                                       */
/* ======================================================================== */

/**
 * @brief   认证请求
 *          { "cmd": 11, "deviceMac": "...", "apiKey": "...", "deviceTimes": "..." }
 */
cJSON *msg_build_auth_request(void);

/* ======================================================================== */
/*  工具                                                                      */
/* ======================================================================== */

char *msg_to_string(cJSON *json);

#ifdef __cplusplus
}
#endif

#endif /* MSG_BUILDER_H */