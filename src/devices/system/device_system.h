/**
 * @file    device_system.h
 * @brief   系统管理命令执行器
 */

#ifndef DEVICE_SYSTEM_H
#define DEVICE_SYSTEM_H

#include "cmd_parser.h"   /* ← 直接包含，cmd_t 已在此定义 */

#ifdef __cplusplus
extern "C" {
#endif

int device_system_execute(const cmd_t *cmd, cJSON **resp);

/**
 * @brief   与国家授时中心 NTP 对时
 *
 *          依次尝试 ntp.ntsc.ac.cn → cn.pool.ntp.org → ntp.aliyun.com，
 *          任意一个成功即返回。ntpdate 未安装时回退到 timedatectl。
 *
 *          fork+exec 实现，多线程安全。
 *
 * @return  0 成功，-1 全部失败
 */
int device_system_ntp_sync(void);

/**
 * @brief   三段式版本号比较
 * @param   a  版本号字符串，如 "1.0.1" 或 "2.0"
 * @param   b  版本号字符串，如 "1.1.0"
 * @return  <0  a < b,  0  a == b,  >0  a > b
 *
 *          逐项比较 major → minor → patch，缺失项视为 0。
 *          空指针视为空字符串，比较结果为 0。
 */
int compare_versions(const char *a, const char *b);

/**
 * @brief   启动看门狗守护进程（fork + setsid，脱离终端独立运行）
 *
 *          看门狗每 WATCHDOG_CHECK_INTERVAL 秒检测 /tmp/device_app.pid
 *          中的主进程是否存活。若主进程死亡：
 *          - 存在 upgrade_pending.json 且 newBin 文件有效 → 替换二进制 → 启动新固件
 *          - 否则 → 直接启动现有固件
 *
 *          重启主进程时设置环境变量 DEVICE_WATCHDOG_ACTIVE=1，
 *          防止 main.c 重复 fork 看门狗。
 *
 *          应在主进程完成基本初始化后调用（确保 /tmp/device_app.pid 已写入）。
 */
void watchdog_daemon_start(void);

/**
 * @brief   写入主进程 PID 到 /tmp/device_app.pid，供看门狗监控
 *
 *          需在看门狗启动之前调用。
 */
void watchdog_write_pid(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_SYSTEM_H */