/**
 * @file    cmd_dispatch.h
 * @brief   命令分发器（V2.6）
 *
 *          根据 cmd_id（整数）查表匹配对应的处理函数。
 *          氛围灯无 cmd 字段时，由 dispatch 层自动识别 state/type 字段转发。
 *
 *          支持 15 条命令（cmd=11, 101~105, 201~202, 301~303, 401, 601~606）
 */

#ifndef CMD_DISPATCH_H
#define CMD_DISPATCH_H

#include "cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  类型定义                                                                  */
/* ======================================================================== */

/**
 * @brief   设备类型枚举（用于命令路由到对应的 worker 线程）
 */
typedef enum {
    DEVICE_NONE    = 0,   /**< 未分类 / 未知                              */
    DEVICE_PTZ     = 1,   /**< 云台                                       */
    DEVICE_LED     = 2,   /**< LED 字幕屏                                  */
    DEVICE_SPEAKER = 3,   /**< 音箱                                       */
    DEVICE_ALARM   = 4,   /**< 警灯                                       */
    DEVICE_SYSTEM  = 5,   /**< 系统管理命令（主线程执行）                    */
} device_type_t;

/**
 * @brief   命令处理函数类型
 * @param   cmd     已解析的命令（可提取 cmd_id、token 及任意字段）
 * @param   resp    输出，回复消息 JSON（调用者负责 cJSON_Delete）
 * @return  0 成功，非 0 失败
 */
typedef int (*cmd_handler_t)(const cmd_t *cmd, cJSON **resp);

/**
 * @brief   命令表条目
 */
typedef struct {
    int              cmd_id;     /**< 命令 ID（如 101）                      */
    cmd_handler_t    handler;    /**< 对应的处理函数                          */
    const char      *desc;       /**< 中文描述                               */
    device_type_t    device;     /**< 所属设备类型（用于多线程路由）           */
} cmd_entry_t;

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

/**
 * @brief   分发命令
 * @param   cmd     已解析的命令
 * @param   resp    输出，回复 JSON
 * @return  0 成功，-1 未匹配
 */
int cmd_dispatch_execute(const cmd_t *cmd, cJSON **resp);

/**
 * @brief   获取命令表（调试/动态注册）
 * @return  命令表数组（以 cmd_id=-1 结尾）
 */
const cmd_entry_t *cmd_dispatch_get_table(void);

/**
 * @brief   根据 cmd_id 查询命令归属的设备类型
 * @param   cmd_id  命令 ID（如 101）
 * @return  设备类型枚举值，未匹配返回 DEVICE_NONE
 */
device_type_t cmd_dispatch_get_device_type(int cmd_id);

#ifdef __cplusplus
}
#endif

#endif /* CMD_DISPATCH_H */
