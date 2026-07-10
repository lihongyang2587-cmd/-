/**
 * @file    task_queue.h
 * @brief   通用线程安全阻塞队列
 *
 *          基于 pthread_mutex_t + pthread_cond_t 实现的固定容量环形队列。
 *          支持多生产者 / 多消费者场景。
 *
 *          典型用法：
 *          - 主线程 push 设备任务到各 worker 队列
 *          - worker 线程 pop 阻塞等待任务
 *          - shutdown() 唤醒所有阻塞的消费者，后续 pop 返回 NULL
 */

#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明类型 */
typedef struct task_queue task_queue_t;

/**
 * @brief   创建阻塞队列
 * @param   capacity    最大容量（条目数）
 * @return  成功返回句柄，失败返回 NULL
 */
task_queue_t *task_queue_create(size_t capacity);

/**
 * @brief   销毁队列并释放所有未取出的元素
 *
 *          【注意】队列中残留的 void* 指针仅被 free 释放，
 *          如果指向复杂结构体，调用者应先 drain 再 destroy。
 *
 * @param   q   队列句柄
 */
void task_queue_destroy(task_queue_t *q);

/**
 * @brief   非阻塞入队
 * @param   q     队列句柄
 * @param   item  要入队的元素指针
 * @return  true 成功，false 队列已满
 */
bool task_queue_push(task_queue_t *q, void *item);

/**
 * @brief   阻塞出队
 * @param   q           队列句柄
 * @param   timeout_ms  超时毫秒数，0 表示无限等待
 * @return  成功返回元素指针，超时或 shutdown 返回 NULL
 */
void *task_queue_pop(task_queue_t *q, int timeout_ms);

/**
 * @brief   关闭队列
 *
 *          设置 shutdown 标志并唤醒所有阻塞的消费者。
 *          后续 push 将被拒绝，pop 将返回 NULL。
 *          调用本函数后不能再使用该队列。
 *
 * @param   q   队列句柄
 */
void task_queue_shutdown(task_queue_t *q);

/**
 * @brief   查询当前队列元素数
 * @param   q   队列句柄
 * @return  当前存储的元素数量
 */
size_t task_queue_count(task_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* TASK_QUEUE_H */
