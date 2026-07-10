/**
 * @file    cmd_parser.h
 * @brief   JSON 命令解析（V2.6）
 *
 *          支持两种入站消息格式：
 *
 *          A. cmd 整数格式（主格式，Server → Board）：
 *             { "cmd": 101, "token": "...", "dir": 1, "speed": 5 }
 *
 *          B. 直接字段格式（氛围灯等少数命令）：
 *             { "state": "on", "type": "breath" }
 *
 *          依赖：cJSON 库
 */

#ifndef CMD_PARSER_H
#define CMD_PARSER_H

#include <stdbool.h>

#include "cJSON.h"   /* 第三方 JSON 解析库 */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  命令 ID 枚举（V2.6）                                                      */
/* ======================================================================== */

enum {
    CMD_AUTH           = 11,    /**< 设备认证                 */
    CMD_PTZ_MOVE       = 101,   /**< 云台方向移动             */
    CMD_PTZ_HOME       = 102,   /**< 云台绝对角度定位         */
    CMD_PTZ_PRESET     = 103,   /**< 云台预置位               */
    CMD_PTZ_CRUISE     = 104,   /**< 云台巡航                 */
    CMD_PTZ_SCAN       = 105,   /**< 云台扫描                 */
    CMD_PTZ_PRESET_QRY = 106,   /**< 云台预置位查询           */
    CMD_LED_TEXT       = 201,   /**< LED 字幕播放             */
    CMD_LED_SWITCH     = 202,   /**< LED 开关                 */
    CMD_SPK_PLAY       = 301,   /**< 音箱播放控制             */
    CMD_SPK_SETLIST    = 302,   /**< 音箱设置音频列表         */
    CMD_SPK_GETLIST    = 303,   /**< 音箱获取音频列表         */
    CMD_ALARM_CTRL     = 401,   /**< 警灯控制                 */
    CMD_SYS_STATUS     = 601,   /**< 系统状态采集             */
    CMD_SYS_TIME       = 602,   /**< 系统时间同步             */
    CMD_SYS_VERSION    = 603,   /**< 系统版本查询             */
    CMD_SYS_UPDATE     = 604,   /**< 系统版本升级             */
    CMD_SYS_CONFIG     = 605,   /**< 系统主服务配置           */
    CMD_SYS_HEARTBEAT  = 606,   /**< 系统心跳（服务器→控制板）*/
    CMD_SYS_EXCEPTION  = 607,   /**< 异常状态上报             */
};

/* ======================================================================== */
/*  类型定义                                                                  */
/* ======================================================================== */

/** 解析后的命令结构 */
typedef struct {
    int      cmd_id;      /**< 命令 ID（如 101），消息无 cmd 字段时为 -1  */
    char    *token;       /**< 鉴权 token（JSON 树内部指针，不可 free）    */
    cJSON   *root;        /**< 原始 JSON 树根，调用者负责 cJSON_Delete     */
} cmd_t;

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

/**
 * @brief   解析 JSON 字符串为 cmd_t 结构
 * @param   json_str    原始 JSON 字符串
 * @param   cmd         输出，解析后的命令结构
 * @return  true 解析成功，false 格式错误
 */
bool cmd_parser_parse(const char *json_str, cmd_t *cmd);

/* ---- 字段提取（直接从 root 取值，兼容 cmd-int 和直接字段两种格式） ---- */

const char *cmd_get_string(const cmd_t *cmd, const char *key);
int         cmd_get_int(const cmd_t *cmd, const char *key, int def);
double      cmd_get_double(const cmd_t *cmd, const char *key, double def);
bool        cmd_get_bool(const cmd_t *cmd, const char *key, bool def);

/** 获取 data 子对象下的字段（保留兼容旧格式） */
const char *cmd_get_data_string(const cmd_t *cmd, const char *key);
int         cmd_get_data_int(const cmd_t *cmd, const char *key, int def);

/**
 * @brief   释放命令结构
 * @param   cmd  命令结构
 */
void cmd_parser_free(cmd_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* CMD_PARSER_H */
