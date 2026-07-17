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
 * @brief   [已废弃] 启动看门狗守护进程
 *
 *          V3.2 起看门狗已迁移为独立 systemd 服务（scripts/device_watchdog.sh），
 *          不再从 device_app 内部 fork。此函数保留仅为编译兼容，不再被调用。
 *
 *          新看门狗部署方式：
 *            sudo cp scripts/device_watchdog.sh /usr/local/bin/
 *            sudo cp scripts/device-watchdog.service /etc/systemd/system/
 *            sudo systemctl enable --now device-watchdog
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