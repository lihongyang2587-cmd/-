/**
 * @file    ring_buffer.c
 * @brief   环形缓冲区实现
 */

#include "ring_buffer.h"

#include <stdlib.h>   /* malloc, free  */
#include <string.h>   /* memset        */
#include <pthread.h>  /* pthread_mutex */

/* ======================================================================== */
/*  内部结构定义                                                              */
/* ======================================================================== */

struct ring_buffer {
    char   **buf;        /* 消息指针数组                 */
    size_t   capacity;   /* 最大容量                     */
    size_t   head;       /* 写入位置（生产者）           */
    size_t   tail;       /* 读取位置（消费者）           */
    size_t   count;      /* 当前消息数                   */

    pthread_mutex_t lock; /* 互斥锁                      */
};

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

ring_buffer_t *ring_buffer_create(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }

    ring_buffer_t *rb = (ring_buffer_t *)malloc(sizeof(ring_buffer_t));
    if (!rb) {
        return NULL;
    }

    rb->buf = (char **)calloc(capacity, sizeof(char *));
    if (!rb->buf) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->head     = 0;
    rb->tail     = 0;
    rb->count    = 0;

    if (pthread_mutex_init(&rb->lock, NULL) != 0) {
        free(rb->buf);
        free(rb);
        return NULL;
    }

    return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) {
        return;
    }

    pthread_mutex_lock(&rb->lock);

    /* 释放所有未被取出的消息 */
    while (rb->count > 0) {
        free(rb->buf[rb->tail]);
        rb->tail = (rb->tail + 1) % rb->capacity;
        rb->count--;
    }

    pthread_mutex_unlock(&rb->lock);
    pthread_mutex_destroy(&rb->lock);

    free(rb->buf);
    free(rb);
}

bool ring_buffer_push(ring_buffer_t *rb, char *msg)
{
    if (!rb || !msg) {
        return false;
    }

    pthread_mutex_lock(&rb->lock);

    if (rb->count >= rb->capacity) {
        /* 缓冲区已满，丢弃最旧的消息（或可改为返回 false） */
        free(rb->buf[rb->tail]);
        rb->tail = (rb->tail + 1) % rb->capacity;
        rb->count--;
    }

    rb->buf[rb->head] = msg;
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count++;

    pthread_mutex_unlock(&rb->lock);
    return true;
}

char *ring_buffer_pop(ring_buffer_t *rb)
{
    if (!rb) {
        return NULL;
    }

    pthread_mutex_lock(&rb->lock);

    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->lock);
        return NULL;
    }

    char *msg = rb->buf[rb->tail];
    rb->buf[rb->tail] = NULL;
    rb->tail = (rb->tail + 1) % rb->capacity;
    rb->count--;

    pthread_mutex_unlock(&rb->lock);
    return msg;
}

size_t ring_buffer_count(ring_buffer_t *rb)
{
    if (!rb) {
        return 0;
    }

    pthread_mutex_lock(&rb->lock);
    size_t n = rb->count;
    pthread_mutex_unlock(&rb->lock);
    return n;
}

bool ring_buffer_is_empty(ring_buffer_t *rb)
{
    if (!rb) {
        return true;
    }

    pthread_mutex_lock(&rb->lock);
    bool empty = (rb->count == 0);
    pthread_mutex_unlock(&rb->lock);
    return empty;
}

bool ring_buffer_is_full(ring_buffer_t *rb)
{
    if (!rb) {
        return false;
    }

    pthread_mutex_lock(&rb->lock);
    bool full = (rb->count >= rb->capacity);
    pthread_mutex_unlock(&rb->lock);
    return full;
}
