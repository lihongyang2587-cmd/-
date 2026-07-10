/**
 * @file    ws_send_queue.c
 * @brief   WebSocket 发送队列实现
 *
 *          内部使用 task_queue 作为底层队列，
 *          启动独立发送线程串行调用 ws_client_send()。
 */

#include "ws_send_queue.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include "task_queue.h"

/* ======================================================================== */
/*  内部结构                                                                  */
/* ======================================================================== */

#define SEND_QUEUE_CAPACITY   128

struct ws_send_queue {
    ws_client_t    *ws;            /* WebSocket 客户端句柄（不拥有）     */
    task_queue_t   *queue;         /* 底层消息队列                      */
    pthread_t       thread;        /* 发送线程                          */
    volatile bool   running;       /* 运行标志                          */
};

/* ======================================================================== */
/*  发送线程                                                                  */
/* ======================================================================== */

static void *sender_thread_func(void *arg)
{
    ws_send_queue_t *sq = (ws_send_queue_t *)arg;

    printf("[SENDQ] 发送线程启动\n");

    while (sq->running) {
        char *msg = (char *)task_queue_pop(sq->queue, 500);
        if (!msg) {
            continue;
        }

        if (ws_client_is_connected(sq->ws)) {
            ws_client_send(sq->ws, msg);
        }
        free(msg);
    }

    /* 退出前排空队列中剩余消息 */
    printf("[SENDQ] 排空剩余消息...\n");
    char *msg;
    while ((msg = (char *)task_queue_pop(sq->queue, 0)) != NULL) {
        if (ws_client_is_connected(sq->ws)) {
            ws_client_send(sq->ws, msg);
        }
        free(msg);
    }

    printf("[SENDQ] 发送线程退出\n");
    return NULL;
}

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

ws_send_queue_t *ws_send_queue_init(ws_client_t *ws)
{
    if (!ws) {
        return NULL;
    }

    ws_send_queue_t *sq = (ws_send_queue_t *)calloc(1, sizeof(ws_send_queue_t));
    if (!sq) {
        return NULL;
    }

    sq->queue = task_queue_create(SEND_QUEUE_CAPACITY);
    if (!sq->queue) {
        free(sq);
        return NULL;
    }

    sq->ws      = ws;
    sq->running = true;

    if (pthread_create(&sq->thread, NULL, sender_thread_func, sq) != 0) {
        task_queue_destroy(sq->queue);
        free(sq);
        return NULL;
    }

    printf("[SENDQ] 初始化完成 (capacity=%d)\n", SEND_QUEUE_CAPACITY);
    return sq;
}

void ws_send_queue_deinit(ws_send_queue_t *sq)
{
    if (!sq) {
        return;
    }

    /* 设置退出标志 */
    sq->running = false;

    /* shutdown 底层队列，唤醒发送线程 */
    task_queue_shutdown(sq->queue);

    /* 等待发送线程退出 */
    pthread_join(sq->thread, NULL);

    /* 销毁底层队列 */
    task_queue_destroy(sq->queue);

    free(sq);
    printf("[SENDQ] 已释放\n");
}

bool ws_send_queue_enqueue(ws_send_queue_t *sq, char *json_str)
{
    if (!sq || !json_str) {
        return false;
    }

    return task_queue_push(sq->queue, (void *)json_str);
}
