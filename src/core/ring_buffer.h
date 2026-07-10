/**
 * @file    ring_buffer.h
 * @brief   线程安全的环形缓冲区
 *
 *          用于在多线程间传递消息指针（char*）。
 *          典型用法：ws_recv 线程 push → 主循环 pop。
 *
 *          线程安全：push/pop 使用互斥锁保护，
 *          支持单生产者单消费者无锁场景的扩展（当前版本用锁实现）。
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h>   /* size_t */
#include <stdbool.h>  /* bool   */

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明类型：环形缓冲区句柄 */
typedef struct ring_buffer ring_buffer_t;

/**
 * @brief   创建环形缓冲区
 * @param   capacity    最大消息条数
 * @return  成功返回句柄，失败返回 NULL
 */
ring_buffer_t *ring_buffer_create(size_t capacity);

/**
 * @brief   销毁环形缓冲区，释放所有未取出的消息
 * @param   rb  缓冲区句柄
 */
void ring_buffer_destroy(ring_buffer_t *rb);

/**
 * @brief   向缓冲区推入一条消息（生产者调用）
 * @param   rb  缓冲区句柄
 * @param   msg 消息字符串，调用者分配，push 后由缓冲区接管所有权
 * @return  true 成功，false 缓冲区已满
 *
 * @note    消息指针所有权转移：push 成功后，调用者不应再释放 msg；
 *          pop 取出后由消费者负责释放。
 */
bool ring_buffer_push(ring_buffer_t *rb, char *msg);

/**
 * @brief   从缓冲区取出一条消息（消费者调用）
 * @param   rb  缓冲区句柄
 * @return  成功返回消息指针（调用者负责 free），缓冲区空时返回 NULL
 */
char *ring_buffer_pop(ring_buffer_t *rb);

/**
 * @brief   查询缓冲区当前消息数
 * @param   rb  缓冲区句柄
 * @return  当前存储的消息条数
 */
size_t ring_buffer_count(ring_buffer_t *rb);

/**
 * @brief   查询缓冲区是否为空
 * @param   rb  缓冲区句柄
 * @return  true 为空
 */
bool ring_buffer_is_empty(ring_buffer_t *rb);

/**
 * @brief   查询缓冲区是否已满
 * @param   rb  缓冲区句柄
 * @return  true 已满
 */
bool ring_buffer_is_full(ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* RING_BUFFER_H */
