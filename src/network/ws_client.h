/**
 * @file    ws_client.h
 * @brief   WebSocket 客户端
 *
 *          基于 mongoose 库实现的 WebSocket 客户端。
 *          负责：
 *          - 连接中心服务器 (ws://192.168.1.100:8080/ws)
 *          - 接收消息 → 推入 ring_buffer
 *          - 发送消息（状态上报、心跳、回复等）
 *          - 断线自动重连
 *
 *          使用方式：
 *          1. ws_client_init(&config) 初始化并启动接收线程
 *          2. 主循环从 ring_buffer 取消息处理
 *          3. ws_client_send(json_str) 发送回复
 *          4. ws_client_deinit() 清理
 */

#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include "ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  类型定义                                                                  */
/* ======================================================================== */

/** 连接状态变化回调 */
typedef void (*ws_state_callback_t)(bool connected, void *user_data);

/** WebSocket 客户端配置 */
typedef struct {
    const char          *server_url;      /**< WebSocket 服务器地址，如 "ws://192.168.1.100:8080/ws" */
    const char          *auth_token;      /**< 鉴权 Token（拼接到 URL）                           */
    ring_buffer_t       *rx_buffer;       /**< 接收消息的环形缓冲区（由调用者创建并传入）          */
    int                  reconnect_sec;   /**< 断线重连间隔（秒）                                 */
    int                  ping_sec;        /**< PING 保活间隔（秒）                                */
    ws_state_callback_t  on_state_change; /**< 连接状态变化时回调（mongoose 线程调用，应快速返回） */
    void                *user_data;       /**< 回调透传数据                                       */
} ws_client_config_t;

/** 不透明句柄 */
typedef struct ws_client ws_client_t;

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

/**
 * @brief   初始化并连接 WebSocket，启动接收线程
 * @param   cfg 配置参数（内部会拷贝，调用者可释放）
 * @return  成功返回句柄，失败返回 NULL
 */
ws_client_t *ws_client_init(const ws_client_config_t *cfg);

/**
 * @brief   发送 JSON 文本消息
 * @param   ws   客户端句柄
 * @param   msg  JSON 字符串（调用者分配，函数内部拷贝后立即返回）
 * @return  true 成功，false 未连接或发送失败
 */
bool ws_client_send(ws_client_t *ws, const char *msg);

/**
 * @brief   查询是否已连接
 * @param   ws  客户端句柄
 * @return  true 已连接
 */
bool ws_client_is_connected(ws_client_t *ws);

/**
 * @brief   更新鉴权 Token（认证成功后调用）
 *
 *          确保断线重连时使用最新 token 而非编译期默认值。
 *          线程安全：可在任意线程调用。
 *
 * @param   ws     客户端句柄
 * @param   token  新的鉴权 Token 字符串（内部拷贝，上限 127 字节）
 */
void ws_client_set_auth_token(ws_client_t *ws, const char *token);

/**
 * @brief   停止接收线程，断开连接，释放资源
 * @param   ws  客户端句柄
 */
void ws_client_deinit(ws_client_t *ws);

#ifdef __cplusplus
}
#endif

#endif /* WS_CLIENT_H */
