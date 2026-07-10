/**
 * @file    main.h
 * @brief   主程序头文件
 *
 *          全局状态和信号处理声明。
 */

#ifndef MAIN_H
#define MAIN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 全局运行标志，收到 SIGINT/SIGTERM 时置 false */
extern volatile bool g_running;

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
