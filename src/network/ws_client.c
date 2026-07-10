/**
 * @file    ws_client.c
 * @brief   WebSocket 客户端实现（基于 mongoose）
 *
 *          依赖：mongoose.c / mongoose.h（单文件库）
 *
 *          架构：mongoose 事件循环在独立线程中运行，
 *          收到 WS 消息时回调 mg_event_handler，写入 ring_buffer。
 */

#include "ws_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "mongoose.h"   /* 第三方 WebSocket 库 */
#include "config.h"

/* ======================================================================== */
/*  内部结构定义                                                              */
/* ======================================================================== */

struct ws_client {
    struct mg_mgr        mgr;            /* mongoose 事件管理器              */
    struct mg_connection *conn;          /* WebSocket 连接                  */
    ring_buffer_t        *rx_buffer;     /* 接收缓冲区（外部传入，不负责释放） */
    char                  server_url[256];/* 服务器 URL 副本                 */
    char                  auth_token[128];/* Token 副本                      */
    int                   reconnect_sec; /* 重连间隔（秒）                   */
    int                   ping_sec;      /* PING 间隔（秒）                  */
    bool                  connected;     /* 当前连接状态                     */
    bool                  running;       /* 事件循环运行标志                  */
    pthread_mutex_t       send_lock;     /* 发送锁：保护 mg_ws_send 调用     */
    pthread_mutex_t       auth_lock;     /* Token 锁：保护 auth_token 读写    */
    ws_state_callback_t   state_cb;      /* 连接状态变化回调                  */
    void                 *cb_user_data;  /* 回调透传数据                     */
};

/* ======================================================================== */
/*  前向声明（mongoose 回调为 static 函数）                                    */
/* ======================================================================== */

static void mg_event_handler(struct mg_connection *c, int ev, void *ev_data);

/* ======================================================================== */
/*  mongoose 事件循环线程                                                      */
/* ======================================================================== */

/**
 * @brief   mongoose 事件循环（运行在独立线程中）
 *
 *          mg_mgr_poll() 阻塞等待事件，超时后返回。
 *          循环直到 running 标志被清除。
 */
static void *mg_event_thread(void *arg)
{
    ws_client_t *ws = (ws_client_t *)arg;

    while (ws->running) {
        mg_mgr_poll(&ws->mgr, 500);  /* 500ms 超时，可及时响应退出 */
    }

    return NULL;
}

/* ======================================================================== */
/*  mongoose 事件回调                                                         */
/* ======================================================================== */

/**
 * @brief   统一的 mongoose 事件处理回调
 *
 *          处理五种事件：
 *          - MG_EV_OPEN：     TCP 连接建立 → 发起 WS 升级握手
 *          - MG_EV_WS_OPEN：  WebSocket 已连接
 *          - MG_EV_WS_MSG：   收到消息 → 写入 ring_buffer
 *          - MG_EV_CLOSE：    连接断开
 *          - MG_EV_ERROR：    连接错误
 */
static void mg_event_handler(struct mg_connection *c, int ev, void *ev_data)
{
    ws_client_t *ws = (ws_client_t *)c->fn_data;

    switch (ev) {

    case MG_EV_OPEN:
        /* 若是 wss:// 连接，先初始化 TLS 再进行 WebSocket 升级 */
        if (c->is_tls) {
            struct mg_tls_opts opts = {.skip_verification = true};
            mg_tls_init(c, &opts);
        }
        printf("[WS] TCP 连接已建立，正在升级...\n");
        /* TCP 已建立，mg_ws_connect 自动发起升级，无需手动处理 */
        break;

    case MG_EV_WS_OPEN:
        printf("[WS] WebSocket 连接已建立\n");
        /* WebSocket 连接已建立 */
        ws->connected = true;
        if (ws->state_cb) {
            ws->state_cb(true, ws->cb_user_data);
        }
        break;

    case MG_EV_WS_MSG:
        /* 收到 WebSocket 消息 */
        {
            struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;

            /* 分配内存并拷贝消息（ring_buffer 接管所有权） */
            size_t len = wm->data.len < (MAX_MSG_LENGTH - 1)
                         ? wm->data.len : (MAX_MSG_LENGTH - 1);
            char *msg = (char *)malloc(len + 1);
            if (msg) {
                memcpy(msg, wm->data.buf, len);
                msg[len] = '\0';

                if (!ring_buffer_push(ws->rx_buffer, msg)) {
                    /* 缓冲区满，丢弃消息 */
                    free(msg);
                }
            }
        }
        break;

    case MG_EV_CLOSE:
        printf("[WS] 连接已关闭\n");
        /* 连接关闭 */
        ws->connected = false;
        ws->conn = NULL;
        if (ws->state_cb) {
            ws->state_cb(false, ws->cb_user_data);
        }
        break;

    case MG_EV_ERROR:
        printf("[WS] 连接错误\n");
        /* 连接错误 */
        ws->connected = false;
        ws->conn = NULL;
        if (ws->state_cb) {
            ws->state_cb(false, ws->cb_user_data);
        }
        break;

    default:
        break;
    }
}

/* ======================================================================== */
/*  重连定时器回调                                                            */
/* ======================================================================== */

/**
 * @brief   重连定时器回调
 *
 *          mongoose 定时器触发时检查连接状态，
 *          若未连接则尝试重新发起 TCP 连接。
 */
static void mg_reconnect_timer(void *arg)
{
    ws_client_t *ws = (ws_client_t *)arg;

    if (!ws->connected && ws->running) {
        pthread_mutex_lock(&ws->auth_lock);
        ws->conn = mg_ws_connect(&ws->mgr, ws->server_url, mg_event_handler, ws,
                         "Auth-Token: %s\r\n", ws->auth_token);
        pthread_mutex_unlock(&ws->auth_lock);
    }
}

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

ws_client_t *ws_client_init(const ws_client_config_t *cfg)
{
    if (!cfg || !cfg->server_url || !cfg->rx_buffer) {
        return NULL;
    }

    ws_client_t *ws = (ws_client_t *)calloc(1, sizeof(ws_client_t));
    if (!ws) {
        return NULL;
    }

    /* 保存配置副本 */
    strncpy(ws->server_url, cfg->server_url, sizeof(ws->server_url) - 1);
    strncpy(ws->auth_token, cfg->auth_token ? cfg->auth_token : "",
            sizeof(ws->auth_token) - 1);
    ws->rx_buffer      = cfg->rx_buffer;
    ws->reconnect_sec  = cfg->reconnect_sec > 0 ? cfg->reconnect_sec : WS_RECONNECT_INTERVAL;
    ws->ping_sec       = cfg->ping_sec > 0 ? cfg->ping_sec : WS_PING_INTERVAL;
    ws->connected      = false;
    ws->running        = true;
    ws->state_cb       = cfg->on_state_change;
    ws->cb_user_data   = cfg->user_data;
    pthread_mutex_init(&ws->send_lock, NULL);
    pthread_mutex_init(&ws->auth_lock, NULL);

    /* 初始化 mongoose */
    mg_mgr_init(&ws->mgr);

    /* 发起连接（auth_token 加锁保护读取） */
    printf("[WS] 发起连接\n");
    pthread_mutex_lock(&ws->auth_lock);
    ws->conn = mg_ws_connect(&ws->mgr, ws->server_url, mg_event_handler, ws,
                         "Auth-Token: %s\r\n", ws->auth_token);
    pthread_mutex_unlock(&ws->auth_lock);


    /* 设置重连定时器 */
    mg_timer_add(&ws->mgr,
                 (uint64_t)ws->reconnect_sec * 1000,
                 MG_TIMER_REPEAT,
                 mg_reconnect_timer,
                 ws);

    /* 启动事件循环线程 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, mg_event_thread, ws) != 0) {
        mg_mgr_free(&ws->mgr);
        free(ws);
        return NULL;
    }
    pthread_detach(tid);

    return ws;
}

bool ws_client_send(ws_client_t *ws, const char *msg)
{
    if (!ws || !ws->conn || !ws->connected || !msg) {
        return false;
    }

    pthread_mutex_lock(&ws->send_lock);
    mg_ws_send(ws->conn, msg, strlen(msg), WEBSOCKET_OP_TEXT);
    pthread_mutex_unlock(&ws->send_lock);
    return true;
}

bool ws_client_is_connected(ws_client_t *ws)
{
    return ws ? ws->connected : false;
}

void ws_client_set_auth_token(ws_client_t *ws, const char *token)
{
    if (!ws || !token) return;

    pthread_mutex_lock(&ws->auth_lock);
    strncpy(ws->auth_token, token, sizeof(ws->auth_token) - 1);
    ws->auth_token[sizeof(ws->auth_token) - 1] = '\0';
    pthread_mutex_unlock(&ws->auth_lock);

    printf("[WS] auth_token 已更新\n");
}

void ws_client_deinit(ws_client_t *ws)
{
    if (!ws) {
        return;
    }

    /* 停止事件循环线程 */
    ws->running = false;

    mg_mgr_free(&ws->mgr);
    pthread_mutex_destroy(&ws->send_lock);
    pthread_mutex_destroy(&ws->auth_lock);
    free(ws);
}
