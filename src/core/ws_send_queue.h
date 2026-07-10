/**
 * @file    ws_send_queue.h
 * @brief   WebSocket 发送队列
 *
 *          封装专用发送线程，串行化所有 ws_client_send() 调用。
 *          mongoose 的 mg_ws_send() 非线程安全，多线程并发发送
 *          需要通过本队列集中管理。
 *
 *          使用方式：
 *          1. ws_send_queue_init(ws_client)  创建并启动发送线程
 *          2. ws_send_queue_enqueue(json_str)  线程安全地入队
 *             （json_str 所有权转移，发送线程负责 free）
 *          3. ws_send_queue_deinit()  排空队列并停止发送线程
 */

#ifndef WS_SEND_QUEUE_H
#define WS_SEND_QUEUE_H

#include <stdbool.h>
#include "ws_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明类型 */
typedef struct ws_send_queue ws_send_queue_t;

/**
 * @brief   创建发送队列并启动发送线程
 * @param   ws  已连接的 WebSocket 客户端句柄
 * @return  成功返回句柄，失败返回 NULL
 */
ws_send_queue_t *ws_send_queue_init(ws_client_t *ws);

/**
 * @brief   停止发送线程、排空队列、释放资源
 * @param   sq  发送队列句柄
 */
void ws_send_queue_deinit(ws_send_queue_t *sq);

/**
 * @brief   线程安全地入队一条待发送 JSON 字符串
 * @param   sq        发送队列句柄
 * @param   json_str  JSON 字符串（所有权转移，调用者不应再 free）
 * @return  true 成功，false 队列满或已 shutdown
 */
bool ws_send_queue_enqueue(ws_send_queue_t *sq, char *json_str);

#ifdef __cplusplus
}
#endif

#endif /* WS_SEND_QUEUE_H */
