/**
 * @file    msg_builder.c
 * @brief   消息构建器实现（V2.6 + 补充特殊响应构建）
 */

#include "msg_builder.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "config_manager.h"

/* ======================================================================== */
/*  内部辅助                                                                  */
/* ======================================================================== */

static cJSON *make_push(const char *event)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", event);

    time_t now = time(NULL);
    char   ts_buf[32];
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    cJSON_AddStringToObject(root, "timestamp", ts_buf);

    return root;
}

/**
 * @brief   自主模式（扫描/巡航）下移除角度字段
 *
 *          merge_status() 只增不改不删，PTZ 进入自主模式后缓存中仍保留
 *          旧的 horizontalAngle/verticalAngle。但自主模式中角度持续变化，
 *          返回无意义，需在输出前剥离。
 */
static void strip_ptz_angles_in_autonomous(cJSON *ptz)
{
    if (!ptz) return;
    cJSON *state = cJSON_GetObjectItem(ptz, "state");
    if (cJSON_IsString(state) &&
        (strcmp(state->valuestring, "scanning") == 0 ||
         strcmp(state->valuestring, "cruising") == 0)) {
        cJSON_DeleteItemFromObject(ptz, "horizontalAngle");
        cJSON_DeleteItemFromObject(ptz, "verticalAngle");
    }
}

/* ======================================================================== */
/*  出站推送消息（Board → Server）                                           */
/* ======================================================================== */

cJSON *msg_build_heartbeat(long uptime_sec)
{
    cJSON *root = make_push("system_heartbeat");
    cJSON *data = cJSON_AddObjectToObject(root, "data");

    cJSON_AddStringToObject(data, "model",      CTRL_BOARD_MODEL);
    cJSON_AddStringToObject(data, "fw_version", config_manager_get_fw_version());
    cJSON_AddNumberToObject(data, "uptime",     (double)uptime_sec);
    cJSON_AddStringToObject(data, "status",     "online");

    return root;
}

cJSON *msg_build_device_status(void)
{
    cJSON *root = make_push("device_status");
    cJSON *data = cJSON_AddObjectToObject(root, "data");

    /*
     * 从配置缓存读取设备状态，不查询硬件，避免与 worker 线程竞争设备互斥锁。
     * 缓存由 config_manager_update_after_cmd() 在每次命令执行后保持同步。
     */
    cJSON_AddItemToObject(data, "ptz",     config_manager_get_ptz_status());
    cJSON_AddItemToObject(data, "led",     config_manager_get_led_status());
    cJSON_AddItemToObject(data, "speaker", config_manager_get_speaker_status());
    cJSON_AddItemToObject(data, "alarm",   config_manager_get_alarm_status());
    cJSON_AddItemToObject(data, "mood",    config_manager_get_mood_status());

    /* 自主模式下移除 ptz 中的角度字段 */
    strip_ptz_angles_in_autonomous(cJSON_GetObjectItem(data, "ptz"));

    return root;
}

cJSON *msg_build_alarm(const char *device_name, const char *message)
{
    cJSON *root = make_push("alarm");
    cJSON *data = cJSON_AddObjectToObject(root, "data");

    cJSON_AddStringToObject(data, "type",    "device");
    cJSON_AddStringToObject(data, "source",  device_name ? device_name : "unknown");
    cJSON_AddStringToObject(data, "message", message ? message : "");

    return root;
}

cJSON *msg_build_error(int code, const char *message)
{
    cJSON *root = make_push("error");
    cJSON *data = cJSON_AddObjectToObject(root, "data");

    cJSON_AddNumberToObject(data, "code",  code);
    cJSON_AddStringToObject(data, "error", message ? message : "");

    return root;
}

cJSON *msg_build_battery(int soc, bool charging)
{
    cJSON *root = make_push("battery");
    cJSON *data = cJSON_AddObjectToObject(root, "data");

    cJSON_AddNumberToObject(data, "soc",      soc);
    cJSON_AddBoolToObject(data,   "charging", charging);

    return root;
}

/* ======================================================================== */
/*  通用 cmd 响应（遗留格式，无 cmd 字段）                                     */
/*  格式: { "code": 0, "msg": "success" }                                    */
/*  V2.7: 新代码请优先使用 msg_build_cmd_response() 以携带 cmd 字段            */
/* ======================================================================== */

cJSON *msg_build_response(int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg",  msg ? msg : "");
    return root;
}

/* ======================================================================== */
/*  通用 cmd 响应（V2.7 统一格式 — 携带 cmd 字段）                             */
/*  格式: { "code": 0, "cmd": N, "msg": "success" }                          */
/* ======================================================================== */

cJSON *msg_build_cmd_response(int code, int cmd, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "cmd",  cmd);
    cJSON_AddStringToObject(root, "msg",  msg ? msg : "");
    return root;
}

/* ======================================================================== */
/*  特殊 cmd 响应（回复中需要携带 cmd 字段及专属数据）                          */
/* ======================================================================== */

/**
 * @brief   cmd=601 状态采集响应
 *          规格书格式: { "cmd": 601, "result": 0, "token": "...",
 *                        "ptz":{...}, "led":{...}, "speaker":{...}, "alarm":{...} }
 */
cJSON *msg_build_sys_status_response(int result, const char *token)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cmd",    601);
    cJSON_AddNumberToObject(root, "result", result);

    /* 回传 token，便于服务端关联请求 */
    if (token && token[0] != '\0') {
        cJSON_AddStringToObject(root, "token", token);
    }

    /*
     * 从配置缓存读取设备状态，不查询硬件，避免阻塞主线程。
     * cmd=601 在主线程内联执行，任何串口 I/O 或 mutex 等待都会冻结
     * 整个主循环，导致无法接收新的 WebSocket 消息。
     *
     * 不含 mood（氛围灯），与接口规格书（测试功能.txt）保持一致。
     */
    cJSON_AddItemToObject(root, "ptz",     config_manager_get_ptz_status());
    cJSON_AddItemToObject(root, "led",     config_manager_get_led_status());
    cJSON_AddItemToObject(root, "speaker", config_manager_get_speaker_status());
    cJSON_AddItemToObject(root, "alarm",   config_manager_get_alarm_status());

    /* 自主模式下移除 ptz 中的角度字段 */
    strip_ptz_angles_in_autonomous(cJSON_GetObjectItem(root, "ptz"));

    return root;
}

/**
 * @brief   cmd=603 版本查询响应
 *          格式: { "cmd": 603, "mainVer": "1.0.0" }
 */
cJSON *msg_build_sys_version_response(const char *version)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cmd",     603);
    cJSON_AddStringToObject(root, "mainVer", version ? version : "");
    return root;
}

/**
 * @brief   cmd=303 获取音频列表响应
 *          格式: { "cmd": 303, "result": 0, "msg": "获取成功",
 *                  "audioNum": N, "audioData": [...] }
 */
cJSON *msg_build_spk_getlist_response(int result, const char *msg,
                                       int audio_num, cJSON *audio_data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cmd",      303);
    cJSON_AddNumberToObject(root, "result",   result);
    cJSON_AddStringToObject(root, "msg",      msg ? msg : "");
    cJSON_AddNumberToObject(root, "audioNum", audio_num);

    if (audio_data) {
        cJSON_AddItemToObject(root, "audioData",
                              cJSON_Duplicate(audio_data, 1));
    } else {
        cJSON_AddItemToObject(root, "audioData", cJSON_CreateArray());
    }
    return root;
}

/* ======================================================================== */
/*  异常状态上报（cmd=607，Board → Server）                                    */
/* ======================================================================== */

cJSON *msg_build_exception_report(int ptz_fail, int led_fail,
                                  int speaker_fail, int alarm_fail)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cmd",              607);
    cJSON_AddStringToObject(root, "token",            "");
    cJSON_AddNumberToObject(root, "ptzFailStatus",    ptz_fail);
    cJSON_AddNumberToObject(root, "ledFailStatus",    led_fail);
    cJSON_AddNumberToObject(root, "speakerFailStatus", speaker_fail);
    cJSON_AddNumberToObject(root, "alarmFailStatus",  alarm_fail);
    return root;
}

/* ======================================================================== */
/*  认证请求（Board → Server，cmd=11）                                       */
/* ======================================================================== */

cJSON *msg_build_auth_request(void)
{
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", lt);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cmd",         11);
    cJSON_AddStringToObject(root, "deviceMac",   DEVICE_MAC);
    cJSON_AddStringToObject(root, "apiKey",      DEVICE_API_KEY);
    cJSON_AddStringToObject(root, "deviceTimes", time_str);

    return root;
}

/* ======================================================================== */
/*  工具                                                                      */
/* ======================================================================== */

char *msg_to_string(cJSON *json)
{
    if (!json) return NULL;
    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return str;
}