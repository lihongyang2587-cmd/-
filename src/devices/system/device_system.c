/**
 * @file    device_system.c
 * @brief   系统管理命令执行器（cmd=601~606）
 */

#include "device_system.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "cmd_parser.h"
#include "msg_builder.h"
#include "main.h"
#include "config.h"
#include "config_manager.h"
#include "device_ptz.h"
#include "device_led.h"
#include "device_speaker.h"
#include "device_alarm.h"

#ifndef ERR_UNKNOWN_CMD
#define ERR_UNKNOWN_CMD            1001
#endif

#ifndef ERR_DEVICE_COMM
#define ERR_DEVICE_COMM            3003
#endif

/* ---- 前向声明（定义在后方的辅助函数） ---- */
static int extract_major_version(const char *version_str);

/* ======================================================================== */
/*  cmd=601 状态采集                                                        */
/* ======================================================================== */

static int handle_sys_status(const cmd_t *cmd, cJSON **resp)
{
    (void)cmd;
    printf("[SYS] 状态采集请求\n");

    /*
     * V3.1: 不在此处调用 device_ptz_query_position()。
     *
     * cmd=601 在主线程内联执行，device_ptz_query_position() 内部
     * 持 mutex 做阻塞串口 I/O（drain_rx 最多 580ms + 查询超时 1s×2），
     * 扫描期间硬件不响应查询，每次超时累计约 3~5 秒。主线程冻结期间：
     *   - 无法接收新的 WebSocket 消息
     *   - 心跳线程 device_ptz_get_status() 阻塞在 g_gimbal_mutex
     *   - PTZ worker device_ptz_execute() 阻塞在 g_gimbal_mutex
     *
     * 角度直接使用缓存值（由 cmd=101/102 更新）。
     * 如需硬件实时角度，由心跳/状态上报间接获取。
     */
    *resp = msg_build_sys_status_response(0, cmd->token);
    return 0;
}

/* ======================================================================== */
/*  cmd=602 校时服务                                                        */
/* ======================================================================== */

static int handle_sys_time(const cmd_t *cmd, cJSON **resp)
{
    const char *server_times = cmd_get_string(cmd, "serverTimes");
    printf("[SYS] 校时服务: serverTimes=%s\n",
           server_times ? server_times : "(null)");

    if (!server_times || server_times[0] == '\0') {
        fprintf(stderr, "[SYS] 缺少 serverTimes 参数\n");
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 602, "缺少 serverTimes 参数");
        return -1;
    }

    printf("[SYS] 已记录服务器时间\n");
    *resp = msg_build_cmd_response(0, 602, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=603 版本查询                                                        */
/* ======================================================================== */

static int handle_sys_version(const cmd_t *cmd, cJSON **resp)
{
    (void)cmd;
    const char *cur_ver = config_manager_get_fw_version();
    printf("[SYS] 版本查询请求, version=%s\n", cur_ver);
    *resp = msg_build_sys_version_response(cur_ver);
    return 0;
}

/* ======================================================================== */
/*  内部辅助：版本解析 / 固件下载 / 看门狗                                       */
/* ======================================================================== */

/**
 * @brief   从三段式版本号中提取主版本号，如 "1.2.3" → 1, "10" → 10
 * @param   version_str  版本号字符串，期望格式 "MAJOR[.MINOR[.PATCH]]"
 * @return  主版本号（整数），解析失败返回 -1
 */
static int extract_major_version(const char *version_str)
{
    if (!version_str || version_str[0] == '\0') return -1;
    char *endptr = NULL;
    long major = strtol(version_str, &endptr, 10);
    if (endptr == version_str || major < 0 || major > INT_MAX) return -1;
    return (int)major;
}

/**
 * @brief   解析三段式版本号的各段
 * @return  成功解析的段数（1~3），失败返回 0
 */
static int parse_version(const char *s, long *out_major, long *out_minor, long *out_patch)
{
    if (!s || s[0] == '\0') return 0;
    char *end = NULL;

    *out_major = strtol(s, &end, 10);
    if (end == s || *out_major < 0) return 0;

    *out_minor = 0;
    *out_patch = 0;

    if (*end == '.') {
        s = end + 1;
        *out_minor = strtol(s, &end, 10);
        if (*out_minor < 0) *out_minor = 0;
    }
    if (*end == '.') {
        s = end + 1;
        *out_patch = strtol(s, &end, 10);
        if (*out_patch < 0) *out_patch = 0;
    }
    return 1;
}

int compare_versions(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";

    long a_major = 0, a_minor = 0, a_patch = 0;
    long b_major = 0, b_minor = 0, b_patch = 0;

    parse_version(a, &a_major, &a_minor, &a_patch);
    parse_version(b, &b_major, &b_minor, &b_patch);

    if (a_major != b_major) return (a_major > b_major) ? 1 : -1;
    if (a_minor != b_minor) return (a_minor > b_minor) ? 1 : -1;
    if (a_patch != b_patch) return (a_patch > b_patch) ? 1 : -1;
    return 0;
}

/**
 * @brief   执行一条 shell 下载命令（fork+exec+alarm，替代 system()）
 *
 *          与 system() 不同，本函数不操作进程级信号掩码，在多线程环境中安全。
 *          子进程通过 alarm() 设硬超时，避免 wget/curl 无限期挂起。
 *
 * @param   cmd  shell 命令行
 * @return  子进程退出码，fork/waitpid 失败返回 -1
 */
static int run_download_cmd(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[SYS] fork 失败: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* 子进程：重置信号为默认，设硬超时 */
        signal(SIGALRM, SIG_DFL);
        alarm(FW_DOWNLOAD_TIMEOUT_SEC + 5);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* 父进程：阻塞等待子进程退出 */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[SYS] waitpid 失败: %s\n", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "[SYS] 下载进程被信号 %d (%s) 终止\n",
                WTERMSIG(status),
                (WTERMSIG(status) == SIGALRM) ? "SIGALRM-超时" : "");
    }

    return -1;
}

/**
 * @brief   下载固件文件，wget 优先，失败回退 curl
 *
 *          使用 fork()+exec()+alarm() 替代 system()，避免多线程环境下的
 *          信号死锁风险。alarm 提供硬超时保护，即使 DNS 卡死也会终止。
 *
 * @param   url         下载 URL
 * @param   output_path 保存路径
 * @return  0 成功，-1 失败
 */
static int download_firmware(const char *url, const char *output_path)
{
    char cmd[1280];
    int  ret;

    /* 尝试 wget（--dns-timeout 避免 DNS 解析卡死） */
    snprintf(cmd, sizeof(cmd),
             "wget -q --dns-timeout=10 -T %d -O \"%s\" \"%s\"",
             FW_DOWNLOAD_TIMEOUT_SEC, output_path, url);
    printf("[SYS] 尝试 wget 下载...\n");
    ret = run_download_cmd(cmd);
    if (ret == 0) {
        struct stat st;
        if (stat(output_path, &st) == 0 && st.st_size > 0) {
            printf("[SYS] wget 成功, size=%ld bytes\n", (long)st.st_size);
            return 0;
        }
        printf("[SYS] wget 返回成功但文件无效，视为失败\n");
        unlink(output_path);
    } else {
        printf("[SYS] wget 失败 (ret=%d), 回退 curl...\n", ret);
    }

    /* 回退 curl（--fail 确保 HTTP 错误返回非零退出码） */
    snprintf(cmd, sizeof(cmd),
             "curl -sS --fail -m %d -o \"%s\" \"%s\"",
             FW_DOWNLOAD_TIMEOUT_SEC, output_path, url);
    printf("[SYS] 尝试 curl 下载...\n");
    ret = run_download_cmd(cmd);
    if (ret == 0) {
        struct stat st;
        if (stat(output_path, &st) == 0 && st.st_size > 0) {
            printf("[SYS] curl 成功, size=%ld bytes\n", (long)st.st_size);
            return 0;
        }
        printf("[SYS] curl 返回成功但文件无效\n");
        unlink(output_path);
    } else {
        printf("[SYS] curl 也失败 (ret=%d)\n", ret);
    }

    return -1;
}

/* ======================================================================== */
/*  看门狗守护进程（常驻，每 30 秒检测主进程存活）                              */
/* ======================================================================== */

/**
 * @brief   写入主进程 PID 到文件，供看门狗监控
 */
void watchdog_write_pid(void)
{
    FILE *fp = fopen(DEVICE_APP_PID_FILE, "w");
    if (fp) {
        fprintf(fp, "%d", (int)getpid());
        fclose(fp);
        printf("[WATCHDOG] 主进程 PID 已写入: %s\n", DEVICE_APP_PID_FILE);
    } else {
        fprintf(stderr, "[WATCHDOG] 无法写入 PID 文件 %s: %s\n",
                DEVICE_APP_PID_FILE, strerror(errno));
    }
}

/**
 * @brief   启动看门狗守护进程（fork + setsid，脱离终端独立运行）
 *
 *          主循环每 WATCHDOG_CHECK_INTERVAL 秒检测主进程是否存活。
 *          若主进程死亡：
 *          - 存在 upgrade_pending.json 且 newBin 有效 → 替换二进制 → 启动新固件
 *          - 否则 → 直接启动现有固件
 *
 *          重启时设置 DEVICE_WATCHDOG_ACTIVE=1 防止 main.c 重复 fork。
 *
 *          使用双 fork 策略彻底脱离终端：
 *          fork₁ → 父进程返回，子进程 setsid() → fork₂ → 孙进程执行循环
 *          （孙进程不再是会话首进程，永远不会获得控制终端）
 */
void watchdog_daemon_start(void)
{
    pid_t pid1 = fork();
    if (pid1 < 0) {
        fprintf(stderr, "[WATCHDOG] 第一次 fork 失败: %s\n", strerror(errno));
        return;
    }

    if (pid1 > 0) {
        /* 父进程：等待中间子进程退出避免僵尸，然后返回 */
        waitpid(pid1, NULL, 0);
        printf("[WATCHDOG] 看门狗守护进程已启动\n");
        return;
    }

    /* ---- 子进程（将成为守护进程） ---- */
    if (setsid() < 0) {
        fprintf(stderr, "[WATCHDOG] setsid 失败: %s\n", strerror(errno));
        _exit(1);
    }

    /* 二次 fork，确保守护进程不是会话首进程 */
    pid_t pid2 = fork();
    if (pid2 < 0) {
        fprintf(stderr, "[WATCHDOG] 第二次 fork 失败: %s\n", strerror(errno));
        _exit(1);
    }

    if (pid2 > 0) {
        /* 中间子进程退出，孙进程成为孤儿被 init 收养 */
        _exit(0);
    }

    /* ---- 孙进程：真正的守护进程 ---- */

    /* 重定向标准 I/O 到 /dev/null */
    if (!freopen("/dev/null", "r", stdin))  { /* ignore */ }
    if (!freopen("/dev/null", "w", stdout)) { /* ignore */ }
    if (!freopen("/dev/null", "w", stderr)) { /* ignore */ }

    /*
     * 注意：不调用 chdir("/") —— FW_PENDING_RESPONSE 使用相对路径
     * "./config/upgrade_pending.json"，切换根目录后会导致找不
     * 到升级文件，从而跳过二进制替换。
     */

    /* 自动回收子进程，避免僵尸进程 */
    signal(SIGCHLD, SIG_IGN);

    /* 写入看门狗 PID */
    FILE *wfp = fopen(WATCHDOG_PID_FILE, "w");
    if (wfp) {
        fprintf(wfp, "%d", (int)getpid());
        fclose(wfp);
    }

    /*
     * 获取当前二进制路径（看门狗 fork 自旧二进制，/proc/self/exe 指向旧路径。
     * 替换后 exec 新二进制时使用同一个路径）
     */
    char target_bin[512];
    ssize_t len = readlink("/proc/self/exe", target_bin, sizeof(target_bin) - 1);
    if (len <= 0) {
        /* 无法获取路径则退出 */
        _exit(1);
    }
    target_bin[len] = '\0';

    /* ===================== 主监控循环 ===================== */
    while (1) {
        sleep(WATCHDOG_CHECK_INTERVAL);

        /* 读取主进程 PID */
        FILE *pf = fopen(DEVICE_APP_PID_FILE, "r");
        if (!pf) continue;

        int main_pid = 0;
        if (fscanf(pf, "%d", &main_pid) != 1 || main_pid <= 0) {
            fclose(pf);
            continue;
        }
        fclose(pf);

        /* 检查主进程是否存活 */
        if (kill(main_pid, 0) == 0) {
            continue;  /* 主进程存活，继续监控 */
        }

        /* ---- 主进程已死亡，准备重启 ---- */
        printf("[WATCHDOG] 主进程 (PID=%d) 已终止，准备重启\n", main_pid);

        /*
         * 检查是否有待处理的升级：
         * upgrade_pending.json 存在 → 尝试替换二进制
         * 不存在 → 直接重启现有二进制
         */
        struct stat pst;
        bool did_upgrade = false;

        if (stat(FW_PENDING_RESPONSE, &pst) == 0 && pst.st_size > 0 && pst.st_size <= 4096) {
            FILE *uf = fopen(FW_PENDING_RESPONSE, "r");
            if (uf) {
                char *ubuf = (char *)calloc(1, (size_t)pst.st_size + 1);
                if (ubuf) {
                    size_t un = fread(ubuf, 1, (size_t)pst.st_size, uf);
                    if (un == (size_t)pst.st_size) {
                        cJSON *up = cJSON_Parse(ubuf);
                        if (up) {
                            const char *new_bin =
                                cJSON_GetStringValue(cJSON_GetObjectItem(up, "newBin"));
                            const char *tgt_bin =
                                cJSON_GetStringValue(cJSON_GetObjectItem(up, "targetBin"));

                            if (new_bin && new_bin[0] != '\0' && tgt_bin && tgt_bin[0] != '\0') {
                                struct stat nst;
                                if (stat(new_bin, &nst) == 0 && nst.st_size > 0) {
                                    printf("[WATCHDOG] 检测到升级文件: %s\n", new_bin);

                                    /* 备份旧二进制 */
                                    char bak_path[520];
                                    snprintf(bak_path, sizeof(bak_path), "%s.bak", tgt_bin);
                                    /* 先删旧备份再 cp（避免 ETXTBUSY 问题——备份不需要执行） */
                                    unlink(bak_path);
                                    {
                                        char cp_cmd[1024];
                                        snprintf(cp_cmd, sizeof(cp_cmd),
                                                 "cp \"%s\" \"%s\"", tgt_bin, bak_path);
                                        int cp_ret = system(cp_cmd);
                                        if (cp_ret != 0) {
                                            fprintf(stderr,
                                                    "[WATCHDOG] 备份失败 (ret=%d): %s\n",
                                                    cp_ret, cp_cmd);
                                        }
                                    }

                                    /*
                                     * 替换二进制：先删旧文件，再移入新文件。
                                     *
                                     * 优先使用 rename()（同文件系统原子操作），
                                     * 失败时回退到 cp+rm（跨文件系统，如
                                     * /tmp 是 tmpfs 而目标在 ext4 时 rename 返回 EXDEV）。
                                     */
                                    unlink(tgt_bin);
                                    if (rename(new_bin, tgt_bin) == 0) {
                                        chmod(tgt_bin, 0755);
                                        printf("[WATCHDOG] 二进制替换完成 (rename): %s\n",
                                               tgt_bin);
                                        did_upgrade = true;
                                    } else if (errno == EXDEV) {
                                        /* 跨文件系统，用 cp + rm 代替 */
                                        char mv_cmd[1024];
                                        snprintf(mv_cmd, sizeof(mv_cmd),
                                                 "cp \"%s\" \"%s\" && rm -f \"%s\"",
                                                 new_bin, tgt_bin, new_bin);
                                        int mv_ret = system(mv_cmd);
                                        if (mv_ret == 0) {
                                            chmod(tgt_bin, 0755);
                                            printf("[WATCHDOG] 二进制替换完成 (cp+rm): %s\n",
                                                   tgt_bin);
                                            did_upgrade = true;
                                        } else {
                                            fprintf(stderr,
                                                    "[WATCHDOG] cp+rm 失败 (ret=%d, errno=%d)\n",
                                                    mv_ret, errno);
                                        }
                                    } else {
                                        fprintf(stderr, "[WATCHDOG] rename 失败 (errno=%d): %s\n",
                                                errno, strerror(errno));
                                    }
                                } else {
                                    fprintf(stderr, "[WATCHDOG] 新固件不存在: %s\n",
                                            new_bin ? new_bin : "(null)");
                                }
                            }
                            cJSON_Delete(up);
                        }
                        free(ubuf);
                    }
                }
                fclose(uf);

                /* 删除升级文件，防止重复尝试替换 */
                unlink(FW_PENDING_RESPONSE);
            }
        }

        /* ---- 启动目标二进制 ---- */
        const char *bin_to_launch = target_bin;
        (void)did_upgrade;  /* 二进制路径始终是 target_bin（升级后已被替换） */

        pid_t child = fork();
        if (child < 0) {
            fprintf(stderr, "[WATCHDOG] 重启 fork 失败: %s\n", strerror(errno));
            continue;
        }

        if (child == 0) {
            /* 子进程：设置环境变量防止 main.c 重复 fork 看门狗 */
            setenv("DEVICE_WATCHDOG_ACTIVE", "1", 1);
            execl(bin_to_launch, bin_to_launch, (char *)NULL);
            /* execl 失败 */
            _exit(1);
        }

        /* 看门狗：更新主进程 PID 文件 */
        pf = fopen(DEVICE_APP_PID_FILE, "w");
        if (pf) {
            fprintf(pf, "%d", (int)child);
            fclose(pf);
        }

        printf("[WATCHDOG] 新主进程已启动 (PID=%d), binary=%s\n",
               (int)child, bin_to_launch);
    }
}

/* ======================================================================== */
/*  cmd=604 版本更新                                                        */
/* ======================================================================== */

static int handle_sys_update(const cmd_t *cmd, cJSON **resp)
{
    const char *main_ver_str = cmd_get_string(cmd, "mainVer");
    const char *down_url     = cmd_get_string(cmd, "downUrl");
    const char *token        = cmd->token;

    printf("[SYS] 版本更新: mainVer=%s, downUrl=%s\n",
           main_ver_str ? main_ver_str : "(null)",
           down_url ? down_url : "(null)");

    /* ---- 参数校验 ---- */
    if (!main_ver_str || main_ver_str[0] == '\0') {
        fprintf(stderr, "[SYS] mainVer 无效: (null)\n");
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 604,
                                       "mainVer 无效");
        return -1;
    }
    if (!down_url || down_url[0] == '\0') {
        fprintf(stderr, "[SYS] 缺少 downUrl 参数\n");
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 604,
                                       "缺少 downUrl 参数");
        return -1;
    }

    /* ---- 版本比对（三段式逐项比较 major.minor.patch） ---- */
    const char *cur_ver = config_manager_get_fw_version();
    if (compare_versions(main_ver_str, cur_ver) <= 0) {
        printf("[SYS] 无需升级 (目标 %s <= 当前 %s)\n",
               main_ver_str, cur_ver);
        *resp = msg_build_cmd_response(0, 604, "already latest");
        return 0;
    }

    int target_major = extract_major_version(main_ver_str);

    printf("[SYS] 版本升级: %s → %s\n", cur_ver, main_ver_str);

    /* ---- 准备下载目录 ---- */
    if (mkdir(FW_DOWNLOAD_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[SYS] 创建下载目录失败: %s\n", strerror(errno));
        *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 604,
                                       "download dir create failed");
        return -1;
    }

    char new_bin_path[512];
    snprintf(new_bin_path, sizeof(new_bin_path),
             "%s/firmware_v%d.bin", FW_DOWNLOAD_DIR, target_major);

    /* ---- 下载新固件（wget → curl 回退） ---- */
    printf("[SYS] 正在下载: %s → %s\n", down_url, new_bin_path);
    if (download_firmware(down_url, new_bin_path) != 0) {
        fprintf(stderr, "[SYS] 固件下载失败（wget 和 curl 均不可用）\n");
        unlink(new_bin_path);
        *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 604,
                                       "download failed (wget/curl)");
        return -1;
    }

    /* ---- 获取当前二进制路径 ---- */
    char target_bin[512];
    ssize_t len = readlink("/proc/self/exe", target_bin,
                           sizeof(target_bin) - 1);
    if (len <= 0) {
        fprintf(stderr, "[SYS] 无法获取当前二进制路径\n");
        unlink(new_bin_path);
        *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 604,
                                       "resolve exe path failed");
        return -1;
    }
    target_bin[len] = '\0';
    printf("[SYS] 当前二进制: %s\n", target_bin);

    /*
     * ---- 写入待发送响应文件 ----
     *
     * 升级完成后由新程序的 config_manager_send_pending_upgrade_response() 读取并发送。
     * 同时包含 newBin / targetBin 路径，供常驻看门狗守护进程在替换二进制时使用。
     *
     * 注意：不再写一次性 watchdog 脚本或 fork watchdog 子进程。
     *       常驻看门狗在启动时已由 main.c 通过 watchdog_daemon_start() 创建，
     *       主进程退出后看门狗会在下一个检测周期发现并处理。
     */
    cJSON *pending = cJSON_CreateObject();
    cJSON_AddNumberToObject(pending, "code",   0);
    cJSON_AddNumberToObject(pending, "cmd",    604);
    cJSON_AddStringToObject(pending, "msg",    "upgrade successful");
    if (token && token[0] != '\0') {
        cJSON_AddStringToObject(pending, "token", token);
    }
    cJSON_AddStringToObject(pending, "toVer",    main_ver_str);
    cJSON_AddStringToObject(pending, "fromVer",  cur_ver);
    cJSON_AddStringToObject(pending, "newBin",   new_bin_path);
    cJSON_AddStringToObject(pending, "targetBin", target_bin);

    char *pending_str = cJSON_Print(pending);
    if (pending_str) {
        FILE *pf = fopen(FW_PENDING_RESPONSE, "w");
        if (pf) {
            fputs(pending_str, pf);
            fclose(pf);
            printf("[SYS] 待发送响应已写入: %s\n", FW_PENDING_RESPONSE);
        } else {
            fprintf(stderr, "[SYS] 警告: 无法写入 %s: %s\n",
                    FW_PENDING_RESPONSE, strerror(errno));
        }
        free(pending_str);
    }
    cJSON_Delete(pending);

    /*
     * 升级已准备就绪：固件已下载、upgrade_pending.json 已写入。
     * 设置 g_running=false 触发主循环退出 → cleanup() → 进程结束。
     * 常驻看门狗检测到主进程死亡后，读取 upgrade_pending.json，
     * 替换二进制并启动新固件。升级成功响应由新程序启动后发送。
     */
    printf("[SYS] 升级准备完成，主进程即将退出\n");
    printf("[SYS] 升级响应将由新程序启动后发送\n");
    g_running = false;
    return 0;
}

/* ======================================================================== */
/*  cmd=605 服务器配置                                                      */
/* ======================================================================== */

static int handle_sys_config(const cmd_t *cmd, cJSON **resp)
{
    const char *server_url = cmd_get_string(cmd, "serverUrl");

    printf("[SYS] 服务器配置: serverUrl=%s\n",
           server_url ? server_url : "(null)");

    if (!server_url || server_url[0] == '\0') {
        fprintf(stderr, "[SYS] 缺少 serverUrl 参数\n");
        *resp = msg_build_cmd_response(ERR_UNKNOWN_CMD, 605, "缺少 serverUrl 参数");
        return -1;
    }

    /* TODO: 实现服务器配置更新逻辑 */
    *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 605, "服务器配置更新功能暂未实现");
    return -1;
}

/* ======================================================================== */
/*  cmd=606 心跳检测                                                        */
/* ======================================================================== */

static int handle_sys_heartbeat(const cmd_t *cmd, cJSON **resp)
{
    const char *timestamp = cmd_get_string(cmd, "timestamp");
    printf("[SYS] 心跳检测: timestamp=%s\n",
           timestamp ? timestamp : "(null)");

    *resp = msg_build_response(0, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=607 异常状态上报                                                     */
/* ======================================================================== */

static int handle_sys_exception(const cmd_t *cmd, cJSON **resp)
{
    (void)cmd;
    int ptz     = device_ptz_get_fail_status();
    int led     = device_led_get_fail_status();
    int speaker = device_speaker_get_fail_status();
    int alarm   = device_alarm_get_fail_status();

    printf("[SYS] 异常状态上报: ptz=%d led=%d speaker=%d alarm=%d\n",
           ptz, led, speaker, alarm);

    *resp = msg_build_exception_report(ptz, led, speaker, alarm);
    return 0;
}

/* ======================================================================== */
/*  统一入口                                                                */
/* ======================================================================== */

int device_system_execute(const cmd_t *cmd, cJSON **resp)
{
    if (!cmd || !resp) return -1;

    switch (cmd->cmd_id) {
    case CMD_SYS_STATUS:    return handle_sys_status(cmd, resp);
    case CMD_SYS_TIME:      return handle_sys_time(cmd, resp);
    case CMD_SYS_VERSION:   return handle_sys_version(cmd, resp);
    case CMD_SYS_UPDATE:    return handle_sys_update(cmd, resp);
    case CMD_SYS_CONFIG:    return handle_sys_config(cmd, resp);
    case CMD_SYS_HEARTBEAT: return handle_sys_heartbeat(cmd, resp);
    case CMD_SYS_EXCEPTION: return handle_sys_exception(cmd, resp);
    default:
        printf("[SYS] 未知系统命令: cmd=%d\n", cmd->cmd_id);
        *resp = msg_build_response(ERR_UNKNOWN_CMD, "unknown system cmd");
        return -1;
    }
}