/**
 * @file    heartbeat.h
 * @brief   心跳定时发送
 *
 *          在独立线程中定时构建心跳和状态消息，通过 ws_client_send 发送。
 *          发送内容：
 *          - system_heartbeat（定时心跳）
 *          - device_status（设备状态，与心跳同频或更低频）
 */

#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <stdbool.h>

#include "ws_client.h"
#include "ws_send_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄 */
typedef struct heartbeat heartbeat_t;

/**
 * @brief   启动心跳定时器
 * @param   ws             WebSocket 客户端句柄
 * @param   send_queue     发送队列（所有消息通过此队列发送）
 * @param   interval_sec   心跳间隔（秒）
 * @return  成功返回句柄，失败返回 NULL
 *
 * @note    内部创建独立线程，定时构建并发送心跳/状态消息。
 */
heartbeat_t *heartbeat_start(ws_client_t *ws, ws_send_queue_t *send_queue,
                              int interval_sec);

/**
 * @brief   停止心跳并释放资源
 * @param   hb  心跳句柄
 */
void heartbeat_stop(heartbeat_t *hb);

#ifdef __cplusplus
}
#endif

#endif /* HEARTBEAT_H */
