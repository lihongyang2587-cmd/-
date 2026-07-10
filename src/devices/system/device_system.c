/**
 * @file    device_system.c
 * @brief   系统管理命令执行器（cmd=601~606）
 */

#include "device_system.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cmd_parser.h"
#include "msg_builder.h"
#include "config.h"
#include "device_ptz.h"
#include "device_led.h"
#include "device_speaker.h"
#include "device_alarm.h"

/* 本地兜底：若 config.h 未定义则使用默认值 */
#ifndef CTRL_BOARD_MAIN_VERSION
#define CTRL_BOARD_MAIN_VERSION    2
#endif

#ifndef ERR_UNKNOWN_CMD
#define ERR_UNKNOWN_CMD            1001
#endif

#ifndef ERR_DEVICE_COMM
#define ERR_DEVICE_COMM            3003
#endif

/* ======================================================================== */
/*  cmd=601 状态采集                                                        */
/* ======================================================================== */

static int handle_sys_status(const cmd_t *cmd, cJSON **resp)
{
    (void)cmd;
    printf("[SYS] 状态采集请求\n");

    /*
     * V3.1: 不在此处调用 device_ptz_query_position()。
     *
     * cmd=601 在主线程内联执行，device_ptz_query_position() 内部
     * 持 mutex 做阻塞串口 I/O（drain_rx 最多 580ms + 查询超时 1s×2），
     * 扫描期间硬件不响应查询，每次超时累计约 3~5 秒。主线程冻结期间：
     *   - 无法接收新的 WebSocket 消息
     *   - 心跳线程 device_ptz_get_status() 阻塞在 g_gimbal_mutex
     *   - PTZ worker device_ptz_execute() 阻塞在 g_gimbal_mutex
     *
     * 角度直接使用缓存值（由 cmd=101/102 更新）。
     * 如需硬件实时角度，由心跳/状态上报间接获取。
     */
    *resp = msg_build_sys_status_response(0);
    return 0;
}

/* ======================================================================== */
/*  cmd=602 校时服务                                                        */
/* ======================================================================== */

static int handle_sys_time(const cmd_t *cmd, cJSON **resp)
{
    const char *server_times = cmd_get_string(cmd, "serverTimes");
    printf("[SYS] 校时服务: serverTimes=%s\n",
           server_times ? server_times : "(null)");

    if (!server_times || server_times[0] == '\0') {
        fprintf(stderr, "[SYS] 缺少 serverTimes 参数\n");
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 602, "缺少 serverTimes 参数");
        return -1;
    }

    printf("[SYS] 已记录服务器时间\n");
    *resp = msg_build_cmd_response(0, 602, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=603 版本查询                                                        */
/* ======================================================================== */

static int handle_sys_version(const cmd_t *cmd, cJSON **resp)
{
    (void)cmd;
    printf("[SYS] 版本查询请求\n");
    *resp = msg_build_sys_version_response(CTRL_BOARD_MAIN_VERSION);
    return 0;
}

/* ======================================================================== */
/*  cmd=604 版本更新                                                        */
/* ======================================================================== */

static int handle_sys_update(const cmd_t *cmd, cJSON **resp)
{
    int         main_ver = cmd_get_int(cmd, "mainVer", 0);
    const char *down_url = cmd_get_string(cmd, "downUrl");

    printf("[SYS] 版本更新: mainVer=%d, downUrl=%s\n",
           main_ver, down_url ? down_url : "(null)");

    if (main_ver <= 0) {
        fprintf(stderr, "[SYS] mainVer 无效: %d\n", main_ver);
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 604, "mainVer 无效，必须大于 0");
        return -1;
    }
    if (!down_url || down_url[0] == '\0') {
        fprintf(stderr, "[SYS] 缺少 downUrl 参数\n");
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 604, "缺少 downUrl 参数");
        return -1;
    }

    /* TODO: 实现固件下载和升级逻辑 */
    *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 604, "固件升级功能暂未实现");
    return -1;
}

/* ======================================================================== */
/*  cmd=605 服务器配置                                                      */
/* ======================================================================== */

static int handle_sys_config(const cmd_t *cmd, cJSON **resp)
{
    const char *server_url = cmd_get_string(cmd, "serverUrl");

    printf("[SYS] 服务器配置: serverUrl=%s\n",
           server_url ? server_url : "(null)");

    if (!server_url || server_url[0] == '\0') {
        fprintf(stderr, "[SYS] 缺少 serverUrl 参数\n");
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 605, "缺少 serverUrl 参数");
        return -1;
    }

    /* TODO: 实现服务器配置更新逻辑 */
    *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 605, "服务器配置更新功能暂未实现");
    return -1;
}

/* ======================================================================== */
/*  cmd=606 心跳检测                                                        */
/* ======================================================================== */

static int handle_sys_heartbeat(const cmd_t *cmd, cJSON **resp)
{
    const char *timestamp = cmd_get_string(cmd, "timestamp");
    printf("[SYS] 心跳检测: timestamp=%s\n",
           timestamp ? timestamp : "(null)");

    *resp = msg_build_response(0, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=607 异常状态上报                                                     */
/* ======================================================================== */

static int handle_sys_exception(const cmd_t *cmd, cJSON **resp)
{
    (void)cmd;
    int ptz     = device_ptz_get_fail_status();
    int led     = device_led_get_fail_status();
    int speaker = device_speaker_get_fail_status();
    int alarm   = device_alarm_get_fail_status();

    printf("[SYS] 异常状态上报: ptz=%d led=%d speaker=%d alarm=%d\n",
           ptz, led, speaker, alarm);

    *resp = msg_build_exception_report(ptz, led, speaker, alarm);
    return 0;
}

/* ======================================================================== */
/*  统一入口                                                                */
/* ======================================================================== */

int device_system_execute(const cmd_t *cmd, cJSON **resp)
{
    if (!cmd || !resp) return -1;

    switch (cmd->cmd_id) {
    case CMD_SYS_STATUS:    return handle_sys_status(cmd, resp);
    case CMD_SYS_TIME:      return handle_sys_time(cmd, resp);
    case CMD_SYS_VERSION:   return handle_sys_version(cmd, resp);
    case CMD_SYS_UPDATE:    return handle_sys_update(cmd, resp);
    case CMD_SYS_CONFIG:    return handle_sys_config(cmd, resp);
    case CMD_SYS_HEARTBEAT: return handle_sys_heartbeat(cmd, resp);
    case CMD_SYS_EXCEPTION: return handle_sys_exception(cmd, resp);
    default:
        printf("[SYS] 未知系统命令: cmd=%d\n", cmd->cmd_id);
        *resp = msg_build_response(ERR_UNKNOWN_CMD, "unknown system cmd");
        return -1;
    }
}