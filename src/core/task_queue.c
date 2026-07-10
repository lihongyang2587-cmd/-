/**
 * @file    task_queue.c
 * @brief   通用线程安全阻塞队列实现
 *
 *          固定容量环形数组 + pthread_mutex_t + pthread_cond_t。
 *          支持多生产者多消费者。
 */

#include "task_queue.h"

#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

/* ======================================================================== */
/*  内部结构                                                                  */
/* ======================================================================== */

struct task_queue {
    void            **buf;         /* 环形数组                            */
    size_t            capacity;    /* 最大容量                            */
    size_t            head;        /* 出队位置                            */
    size_t            tail;        /* 入队位置                            */
    size_t            count;       /* 当前元素数                          */
    bool              shutdown;    /* 关闭标志，唤醒所有阻塞消费者         */
    pthread_mutex_t   lock;        /* 互斥锁                              */
    pthread_cond_t    not_empty;   /* 非空条件变量                        */
    pthread_cond_t    not_full;    /* 非满条件变量                        */
};

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

task_queue_t *task_queue_create(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }

    task_queue_t *q = (task_queue_t *)calloc(1, sizeof(task_queue_t));
    if (!q) {
        return NULL;
    }

    q->buf = (void **)calloc(capacity, sizeof(void *));
    if (!q->buf) {
        free(q);
        return NULL;
    }

    q->capacity = capacity;
    q->head     = 0;
    q->tail     = 0;
    q->count    = 0;
    q->shutdown = false;

    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);

    return q;
}

void task_queue_destroy(task_queue_t *q)
{
    if (!q) {
        return;
    }

    /*
     * 释放队列中残留的元素指针。
     * 注意：仅做 free(ptr)，如果元素指向复杂结构体，
     * 调用者应先 drain 再 destroy。
     */
    for (size_t i = 0; i < q->count; i++) {
        size_t idx = (q->head + i) % q->capacity;
        free(q->buf[idx]);
    }

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);

    free(q->buf);
    free(q);
}

bool task_queue_push(task_queue_t *q, void *item)
{
    if (!q || !item) {
        return false;
    }

    pthread_mutex_lock(&q->lock);

    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    if (q->count >= q->capacity) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    q->buf[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    /* 唤醒一个等待的消费者 */
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);

    return true;
}

void *task_queue_pop(task_queue_t *q, int timeout_ms)
{
    if (!q) {
        return NULL;
    }

    pthread_mutex_lock(&q->lock);

    /* 等待直到有元素或 shutdown */
    while (q->count == 0 && !q->shutdown) {
        if (timeout_ms > 0) {
            struct timeval  now;
            struct timespec ts;
            gettimeofday(&now, NULL);
            ts.tv_sec  = now.tv_sec + (now.tv_usec / 1000000 + timeout_ms) / 1000;
            ts.tv_nsec = ((now.tv_usec * 1000) + (timeout_ms % 1000) * 1000000) % 1000000000;
            /* 处理纳秒进位 */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec  += 1;
                ts.tv_nsec -= 1000000000;
            }
            int ret = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&q->lock);
                return NULL;
            }
        } else {
            /* timeout_ms == 0: 无限等待 */
            pthread_cond_wait(&q->not_empty, &q->lock);
        }
    }

    /* shutdown 且队列空 → 返回 NULL */
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    void *item = q->buf[q->head];
    q->buf[q->head] = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    /* 唤醒一个等待的生产者 */
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);

    return item;
}

void task_queue_shutdown(task_queue_t *q)
{
    if (!q) {
        return;
    }

    pthread_mutex_lock(&q->lock);
    q->shutdown = true;
    /* 唤醒所有阻塞的消费者 */
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

size_t task_queue_count(task_queue_t *q)
{
    if (!q) {
        return 0;
    }

    pthread_mutex_lock(&q->lock);
    size_t cnt = q->count;
    pthread_mutex_unlock(&q->lock);
    return cnt;
}
