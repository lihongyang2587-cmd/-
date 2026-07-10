/**
 * @file    cmd_dispatch.c
 * @brief   命令分发器实现（V2.6）
 *
 *          15 条命令全部注册。氛围灯（无 cmd）通过直接字段识别。
 */

#include "cmd_dispatch.h"

#include <string.h>
#include <stdio.h>

#include "config.h"
#include "msg_builder.h"

/* 设备模块 */
#include "device_ptz.h"
#include "device_led.h"
#include "device_speaker.h"
#include "device_alarm.h"
#include "device_mood.h"
#include "device_auth.h"
#include "device_system.h"

/* ======================================================================== */
/*  命令表（V2.6 — 15 条命令）                                                */
/* ======================================================================== */

static const cmd_entry_t cmd_table[] = {

    /* ---- 认证 ---- */
    { CMD_AUTH,          device_auth_execute,    "设备认证",             DEVICE_SYSTEM  },

    /* ---- 云台 cmd=101~105 ---- */
    { CMD_PTZ_MOVE,      device_ptz_execute,     "云台方向移动",         DEVICE_PTZ     },
    { CMD_PTZ_HOME,      device_ptz_execute,     "云台绝对角度定位",     DEVICE_PTZ     },
    { CMD_PTZ_PRESET,    device_ptz_execute,     "云台预置位",           DEVICE_PTZ     },
    { CMD_PTZ_CRUISE,    device_ptz_execute,     "云台巡航",             DEVICE_PTZ     },
    { CMD_PTZ_SCAN,      device_ptz_execute,     "云台扫描",             DEVICE_PTZ     },
    { CMD_PTZ_PRESET_QRY, device_ptz_execute,    "云台预置位查询",       DEVICE_PTZ     },

    /* ---- LED 字幕屏 cmd=201~202 ---- */
    { CMD_LED_TEXT,      device_led_execute,     "LED 字幕播放",         DEVICE_LED     },
    { CMD_LED_SWITCH,    device_led_execute,     "LED 开关",             DEVICE_LED     },

    /* ---- 音箱 cmd=301~303 ---- */
    { CMD_SPK_PLAY,      device_speaker_execute, "音箱播放控制",         DEVICE_SPEAKER },
    { CMD_SPK_SETLIST,   device_speaker_execute, "音箱设置音频列表",     DEVICE_SPEAKER },
    { CMD_SPK_GETLIST,   device_speaker_execute, "音箱获取音频列表",     DEVICE_SPEAKER },

    /* ---- 警灯 cmd=401 ---- */
    { CMD_ALARM_CTRL,    device_alarm_execute,   "警灯控制",             DEVICE_ALARM   },

    /* ---- 系统管理 cmd=601~607 ---- */
    { CMD_SYS_STATUS,    device_system_execute,  "系统状态采集",         DEVICE_SYSTEM  },
    { CMD_SYS_TIME,      device_system_execute,  "系统时间同步",         DEVICE_SYSTEM  },
    { CMD_SYS_VERSION,   device_system_execute,  "系统版本查询",         DEVICE_SYSTEM  },
    { CMD_SYS_UPDATE,    device_system_execute,  "系统版本升级",         DEVICE_SYSTEM  },
    { CMD_SYS_CONFIG,    device_system_execute,  "系统主服务配置",       DEVICE_SYSTEM  },
    { CMD_SYS_HEARTBEAT, device_system_execute,  "系统心跳",             DEVICE_SYSTEM  },
    { CMD_SYS_EXCEPTION, device_system_execute,  "异常状态上报",         DEVICE_SYSTEM  },

    /* ---- 哨兵 ---- */
    { -1, NULL, NULL, DEVICE_NONE },
};

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

int cmd_dispatch_execute(const cmd_t *cmd, cJSON **resp)
{
    if (!cmd || !cmd->root) {
        return -1;
    }

    printf("[DISPATCH] cmd=%d\n", cmd->cmd_id);

    /* ---- 氛围灯特殊处理：无 cmd 字段，通过 state/type 识别 ---- */
    if (cmd->cmd_id < 0) {
        cJSON *state_item = cJSON_GetObjectItem(cmd->root, "state");
        if (cJSON_IsString(state_item)) {
            printf("[DISPATCH] 识别为氛围灯指令 (无cmd, 有state字段)\n");
            return device_mood_execute(cmd, resp);
        }
        /* 其他无 cmd 消息（如心跳回复等），忽略 */
        printf("[DISPATCH] 无cmd消息，忽略\n");
        return -1;
    }

    /* ---- 整数 cmd 查表 ---- */
    for (const cmd_entry_t *e = cmd_table; e->cmd_id >= 0; e++) {
        if (cmd->cmd_id == e->cmd_id) {
            printf("[DISPATCH] 匹配: cmd=%d → %s\n", e->cmd_id, e->desc);
            int ret = e->handler(cmd, resp);
            if (ret != 0) {
                printf("[DISPATCH] 命令失败: cmd=%d ret=%d\n", e->cmd_id, ret);
            }
            return ret;
        }
    }

    printf("[DISPATCH] 未知命令: cmd=%d\n", cmd->cmd_id);
    if (resp) {
        *resp = msg_build_cmd_response(1001, cmd->cmd_id, "未知命令");
    }
    return -1;
}

const cmd_entry_t *cmd_dispatch_get_table(void)
{
    return cmd_table;
}

device_type_t cmd_dispatch_get_device_type(int cmd_id)
{
    for (const cmd_entry_t *e = cmd_table; e->cmd_id >= 0; e++) {
        if (cmd_id == e->cmd_id) {
            return e->device;
        }
    }
    return DEVICE_NONE;
}
