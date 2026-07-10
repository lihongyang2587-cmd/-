/**
 * @file    heartbeat.c
 * @brief   心跳定时发送实现
 *
 *          在独立线程中定时：
 *          1. 构建心跳消息 (system_heartbeat)
 *          2. 构建设备状态消息 (device_status)
 *          3. 通过 ws_client_send 发送
 */

#include "heartbeat.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>    /* sleep */
#include <time.h>      /* time  */
#include <pthread.h>

#include "msg_builder.h"
#include "config.h"

/* ======================================================================== */
/*  内部结构                                                                  */
/* ======================================================================== */

struct heartbeat {
    ws_client_t     *ws;            /* WebSocket 客户端句柄（不负责释放） */
    ws_send_queue_t *send_queue;    /* 发送队列（不负责释放）             */
    int              interval_sec;  /* 心跳间隔（秒）                    */
    bool             running;       /* 运行标志                          */
};

/* ======================================================================== */
/*  线程函数                                                                  */
/* ======================================================================== */

static void *heartbeat_thread(void *arg)
{
    heartbeat_t *hb = (heartbeat_t *)arg;
    time_t       start_time = time(NULL);

    while (hb->running) {
        sleep((unsigned int)hb->interval_sec);

        if (!hb->running) break;

        /* 1. 发送心跳 */
        printf("[HEARTBEAT] 发送心跳\n");
        long uptime = (long)(time(NULL) - start_time);
        cJSON *hb_json = msg_build_heartbeat(uptime);
        char  *hb_str  = msg_to_string(hb_json);
        if (hb_str) {
            ws_send_queue_enqueue(hb->send_queue, hb_str);
            /* hb_str 所有权已转移到 send_queue */
        }

        /* 2. 发送设备状态 */
        cJSON *status_json = msg_build_device_status();
        char  *status_str  = msg_to_string(status_json);
        if (status_str) {
            ws_send_queue_enqueue(hb->send_queue, status_str);
            /* status_str 所有权已转移到 send_queue */
        }
    }

    return NULL;
}

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

heartbeat_t *heartbeat_start(ws_client_t *ws, ws_send_queue_t *send_queue,
                              int interval_sec)
{
    if (!ws || !send_queue) {
        return NULL;
    }

    heartbeat_t *hb = (heartbeat_t *)calloc(1, sizeof(heartbeat_t));
    if (!hb) {
        return NULL;
    }

    hb->ws           = ws;
    hb->send_queue   = send_queue;
    hb->interval_sec = interval_sec > 0 ? interval_sec : HEARTBEAT_INTERVAL;
    hb->running      = true;

    pthread_t tid;
    if (pthread_create(&tid, NULL, heartbeat_thread, hb) != 0) {
        free(hb);
        return NULL;
    }
    pthread_detach(tid);

    return hb;
}

void heartbeat_stop(heartbeat_t *hb)
{
    if (hb) {
        hb->running = false;
        free(hb);
    }
}
