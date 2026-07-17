/**
 * @file    device_speaker.c
 * @brief   音箱控制实现（V2.6 完整版）
 *
 *          底层播放: WAV → aplay，MP3 → mpg123
 *          USB 声卡: 启动时自动检测（/proc/asound/cards），fallback card 1
 *          音量控制: amixer -c <card> set PCM <vol>%
 *          文件下载: wget（cmd=302）
 *          音乐目录: /home/cat/websocket/music/
 *
 *  依赖:
 *    - mpg123  (apt install mpg123)  — MP3 播放
 *    - alsa-utils (aplay/amixer)     — WAV 播放 + 音量控制
 *    - wget                           — 音频文件下载
 */

#define _GNU_SOURCE

#include "device_speaker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

#include "msg_builder.h"
#include "config.h"

/* ======================================================================== */
/*  常量定义                                                                  */
/* ======================================================================== */

#define MUSIC_DIR           "/root/music/"
#define MAX_AUDIO_FILES     100
#define MAX_SEQ_LEN         50
#define DEFAULT_ALSA_CARD   1
#define DEFAULT_VOLUME      50
#define LOOP_GAP_US         500000      /* 循环播放间隔 0.5s */
#define SEQ_GAP_US          500000      /* 顺序播放间隔 0.5s */
#define POLL_INTERVAL_US    200000      /* 子进程轮询间隔 200ms */
#define MAX_PLAY_RETRY      3           /* aplay 异常退出最多重试次数 */
#define RETRY_DELAY_US      500000      /* 重试前等待 500ms */

#ifndef ERR_SUCCESS
#define ERR_SUCCESS         0
#endif

#ifndef ERR_PARAM_INVALID
#define ERR_PARAM_INVALID   1001
#endif

#ifndef ERR_DEVICE_COMM
#define ERR_DEVICE_COMM     3003
#endif

/* ======================================================================== */
/*  音频列表存储                                                               */
/* ======================================================================== */

typedef struct {
    int  valid;
    int  index;
    char audio_name[128];
    char local_path[512];       /* 完整本地路径 */
} audio_entry_t;

static audio_entry_t g_audio_list[MAX_AUDIO_FILES];

/* ======================================================================== */
/*  播放请求与状态                                                              */
/* ======================================================================== */

typedef struct {
    int  play_type;             /* 1=循环, 2=顺序, 3=定时, 4=单次/插播 */
    int  audio_index;           /* type 1/3/4 使用 */
    int  seq[MAX_SEQ_LEN];      /* type 2: 按序排列的 audio index */
    int  seq_count;             /* type 2: 序列长度 */
    char timed_str[32];         /* type 3: "YYYY-MM-DD HH:mm:ss" */
} play_request_t;

static int             g_alsa_card        = DEFAULT_ALSA_CARD;
static int             g_volume           = DEFAULT_VOLUME;
static int             g_playing          = 0;       /* 正在播放 */
static int             g_paused           = 0;       /* 已暂停 */
static int             g_current_audio    = -1;      /* 当前播放的 audio index */
static volatile pid_t  g_player_pid       = -1;      /* 当前播放子进程（volatile：多线程读写） */
static pthread_t       g_play_thread;
static volatile int    g_thread_active    = 0;
static volatile int    g_stop_flag        = 0;       /* 通知播放线程退出 */
static volatile int    g_insert_active    = 0;       /* 插播进行中，防止嵌套插播 */
static volatile int    g_status_changed   = 0;
static play_request_t  g_request;                     /* 当前播放请求 */

/* V2.7 状态追踪（供 get_status 上报） */
static int             g_play_type        = 0;       /* 播放模式 1-4 */
static int             g_play_data        = 0;       /* 播放数据（顺序=条数，其他=0/1） */
static int             g_fail_status      = 0;       /* 0=正常, 1=不在线, 2=响应异常 */

/* 互斥锁，保护状态变量的并发读写（worker 线程 + 心跳线程） */
static pthread_mutex_t g_spk_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 状态变更回调（可选，当前主要靠标志位轮询） */
typedef void (*speaker_status_cb_t)(void);
static speaker_status_cb_t g_status_cb = NULL;

/* ======================================================================== */
/*  工具函数                                                                   */
/* ======================================================================== */

static void notify_status_changed(void)
{
    g_status_changed = 1;
    if (g_status_cb) g_status_cb();
}

/**
 * @brief   确保音乐目录存在
 */
static void ensure_music_dir(void)
{
    struct stat st;
    if (stat(MUSIC_DIR, &st) != 0) {
        if (mkdir(MUSIC_DIR, 0755) == 0) {
            printf("[SPK] 创建音乐目录: %s\n", MUSIC_DIR);
        } else {
            fprintf(stderr, "[SPK] 创建目录失败: %s: %s\n",
                    MUSIC_DIR, strerror(errno));
        }
    }
}

/**
 * @brief   从 /proc/asound/cards 检测 USB 声卡编号
 * @return  声卡编号，未找到返回 DEFAULT_ALSA_CARD
 */
static int detect_usb_card(void)
{
    FILE *fp = fopen("/proc/asound/cards", "r");
    if (!fp) {
        printf("[SPK] 无法读取 /proc/asound/cards，使用默认 card %d\n",
               DEFAULT_ALSA_CARD);
        return DEFAULT_ALSA_CARD;
    }

    /*
     * 匹配优先级：
     *   1. jieli / UACDemo     — Jieli 系列 USB 声卡
     *   2. USB-Audio / USB     — 通用 USB 音频设备
     *   3. 兜底 DEFAULT_ALSA_CARD
     */
    printf("[SPK] === 扫描声卡 ===\n");
    char line[256];
    int card          = -1;      /* 最高优先级匹配 */
    int card_fallback = -1;      /* 通用 USB 兜底匹配 */
    int last_card     = -1;      /* 最近解析到的卡号 */

    while (fgets(line, sizeof(line), fp)) {
        printf("[SPK]   %s", line);

        /*
         * 提取卡号：/proc/asound/cards 每行开头可能有前导空格，
         * sscanf %d 自动跳过空白字符，不能靠 line[0] 判数字。
         */
        int num;
        if (sscanf(line, "%d", &num) == 1) {
            last_card = num;
        }

        /* 转小写 */
        char lower[256];
        snprintf(lower, sizeof(lower), "%s", line);
        for (int i = 0; lower[i]; i++) {
            lower[i] = tolower((unsigned char)lower[i]);
        }

        if (last_card < 0) continue;

        /* P1: Jieli / UACDemo 精确匹配 */
        if (strstr(lower, "jieli") || strstr(lower, "uacdemo")) {
            card = last_card;
        }
        /* P2: 通用 USB 音频（仅当 P1 未命中时） */
        if (card < 0 && (strstr(lower, "usb-audio") || strstr(lower, "usb audio"))) {
            card_fallback = last_card;
        }
    }
    fclose(fp);

    /* 返回最优匹配 */
    int result = -1;
    if (card >= 0) {
        printf("[SPK] 匹配到 Jieli/UACDemo 声卡: card %d\n", card);
        result = card;
    } else if (card_fallback >= 0) {
        printf("[SPK] 匹配到 USB 音频设备: card %d\n", card_fallback);
        result = card_fallback;
    } else {
        printf("[SPK] 未找到 USB 声卡，使用默认 card %d\n", DEFAULT_ALSA_CARD);
        result = DEFAULT_ALSA_CARD;
    }

    /*
     * 兜底：如果 card 0 存在且 detect 失败，尝试 card 1。
     * 很多 RK3588 板载声卡占 card 0，USB 插上后就是 card 1。
     */
    if (result == DEFAULT_ALSA_CARD && result == 1) {
        printf("[SPK] 使用 card 1（常见 USB 声卡位置）\n");
    }

    return result;
}
/**
 * @brief   设置音量
 */
static void set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100)  vol = 100;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "amixer -c %d set PCM %d%% > /dev/null 2>&1",
             g_alsa_card, vol);
    int ret = system(cmd);
    (void)ret;
    g_volume = vol;
    printf("[SPK] 音量设置: %d%%\n", vol);
}

/**
 * @brief   根据索引获取本地文件路径
 */
static const char *get_audio_path(int index)
{
    if (index < 0 || index >= MAX_AUDIO_FILES) return NULL;
    if (!g_audio_list[index].valid) return NULL;
    return g_audio_list[index].local_path;
}

/**
 * @brief   终止播放子进程（仅发信号，不 wait——由 play_file_blocking 统一回收）
 */
static void kill_player(void)
{
    if (g_player_pid > 0) {
        /* 如果子进程处于 STOP 状态，先 CONT 再 TERM */
        kill(g_player_pid, SIGCONT);
        usleep(10000);
        kill(g_player_pid, SIGTERM);
        usleep(100000);
        /* 如果还没退出，KILL */
        if (kill(g_player_pid, 0) == 0) {
            kill(g_player_pid, SIGKILL);
        }
        /* 不 waitpid —— 由 play_file_blocking() 回收子进程 */
    }
}

/**
 * @brief   回收旧播放线程（阻塞等待线程结束）
 */
static void join_play_thread(void)
{
    if (g_thread_active) {
        g_stop_flag = 1;
        kill_player();          /* 唤醒等待中的线程 */
        pthread_join(g_play_thread, NULL);
        g_thread_active = 0;
        g_stop_flag = 0;
    }
}

/* ======================================================================== */
/*  播放核心                                                                   */
/* ======================================================================== */

/**
 * @brief   播放单个文件（阻塞，直到播完或被 g_stop_flag 中断）
 *
 *          WAV → aplay -D plughw:<card>,0 <file>
 *          MP3 → mpg123 -q -a plughw:<card>,0 <file>
 *
 *          子进程暂停（SIGSTOP）期间 waitpid(WNOHANG) 返回 0，
 *          循环继续等待，不会误判为退出。
 *
 * @return  0 正常播完，-1 被中断或出错
 */
static int play_file_blocking(const char *filepath)
{
    if (!filepath || access(filepath, F_OK) != 0) {
        fprintf(stderr, "[SPK] 文件不存在: %s\n",
                filepath ? filepath : "(null)");
        return -1;
    }

    /* 判断文件类型 */
    const char *dot = strrchr(filepath, '.');
    int is_wav = (dot && strcasecmp(dot, ".wav") == 0);

    for (int retry = 0; retry < MAX_PLAY_RETRY; retry++) {
        if (g_stop_flag) return -1;

        if (retry > 0) {
            printf("[SPK] 重试播放 (%d/%d): %s\n",
                   retry + 1, MAX_PLAY_RETRY, filepath);
            usleep(RETRY_DELAY_US);
        } else {
            printf("[SPK] 播放: %s (%s)\n", filepath, is_wav ? "WAV" : "MP3");
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("[SPK] fork 失败");
            return -1;
        }
        if (pid == 0) {
            /*
             * 子进程：直接 exec 播放器，不通过 shell。
             * stderr 保留原样不重定向，确保 aplay/mpg123 的错误信息可见。
             */
            char device[64];
            snprintf(device, sizeof(device), "plughw:%d,0", g_alsa_card);

            if (is_wav) {
                execlp("aplay", "aplay", "-D", device, filepath, NULL);
            } else {
                execlp("mpg123", "mpg123", "-q", "-a", device, filepath, NULL);
            }
            /* execlp 失败才走到这里 */
            fprintf(stderr, "[SPK] exec 失败: %s (errno=%d, %s)\n",
                    is_wav ? "aplay" : "mpg123", errno, strerror(errno));
            _exit(127);
        }

        g_player_pid = pid;
        g_playing = 1;
        g_fail_status = 0;
        notify_status_changed();

        /* 等待子进程，定期检查 g_stop_flag */
        int status;
        int should_retry = 0;

        while (!g_stop_flag) {
            pid_t re = waitpid(pid, &status, WNOHANG | WUNTRACED);
            if (re == pid) {
                if (WIFSTOPPED(status)) {
                    /* 子进程被 SIGSTOP 暂停，继续轮询等待恢复或终止 */
                    usleep(POLL_INTERVAL_US);
                    continue;
                }
                /* 子进程已退出 */
                g_player_pid = -1;
                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    if (code == 0) {
                        g_fail_status = 0;
                        printf("[SPK] 播放完成: %s\n", filepath);
                        return 0;
                    }
                    printf("[SPK] 播放异常退出: %s (exit=%d)\n", filepath, code);
                    if (retry < MAX_PLAY_RETRY - 1) {
                        should_retry = 1;
                    }
                } else if (WIFSIGNALED(status)) {
                    printf("[SPK] 播放被信号终止: %s (sig=%d)\n",
                           filepath, WTERMSIG(status));
                }
                break;  /* 退出 wait 循环 */
            }
            if (re < 0) {
                g_player_pid = -1;
                printf("[SPK] waitpid 错误: %s (errno=%d)\n", filepath, errno);
                g_fail_status = 2;
                return -1;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (should_retry) continue;  /* 下一轮重试 */

        /* 被要求停止 → 杀掉子进程 */
        if (g_stop_flag) {
            if (kill(pid, 0) == 0) {
                kill(pid, SIGCONT);         /* 若处于 STOP 状态先唤醒 */
                usleep(10000);
                kill(pid, SIGTERM);
                usleep(100000);
                if (kill(pid, 0) == 0) kill(pid, SIGKILL);
            }
            waitpid(pid, &status, 0);
            g_player_pid = -1;
            printf("[SPK] 播放已停止: %s\n", filepath);
        }

        g_fail_status = 2;
        return -1;
    }

    /* 所有重试已耗尽 */
    g_fail_status = 2;
    return -1;
}

/**
 * @brief   解析 "YYYY-MM-DD HH:mm:ss" 时间字符串，校验各字段范围
 * @return  0 成功，-1 失败
 */
static int parse_datetime(const char *str, struct tm *out_tm)
{
    if (!str || !out_tm) return -1;

    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

    if (sscanf(str, "%d-%d-%d %d:%d:%d",
               &year, &month, &day, &hour, &min, &sec) != 6) {
        fprintf(stderr, "[SPK] 时间格式解析失败（字段不足 6 个）: %s\n", str);
        return -1;
    }

    if (year  < 1970 || year  > 2099) return -1;
    if (month < 1    || month > 12)   return -1;
    if (day   < 1    || day   > 31)   return -1;
    if (hour  < 0    || hour  > 23)   return -1;
    if (min   < 0    || min   > 59)   return -1;
    if (sec   < 0    || sec   > 59)   return -1;

    memset(out_tm, 0, sizeof(struct tm));
    out_tm->tm_year  = year  - 1900;
    out_tm->tm_mon   = month - 1;
    out_tm->tm_mday  = day;
    out_tm->tm_hour  = hour;
    out_tm->tm_min   = min;
    out_tm->tm_sec   = sec;
    out_tm->tm_isdst = -1;

    return 0;
}

/**
 * @brief   等待到指定时间（同 alarm 模块定时逻辑）
 * @return  0 到达目标时间，-1 解析失败
 */
static int wait_until_time(const char *time_str)
{
    struct tm target;
    if (parse_datetime(time_str, &target) != 0) {
        fprintf(stderr, "[SPK] 定时时间解析失败: %s\n", time_str);
        return -1;
    }

    time_t target_time = mktime(&target);
    if (target_time == (time_t)-1) {
        fprintf(stderr, "[SPK] mktime 转换失败\n");
        return -1;
    }

    /* 防御性检查：正常情况下 cmd 层已拦截过期时间，此处作为最后防线 */
    {
        time_t now;
        time(&now);
        long diff_sec = (long)difftime(target_time, now);
        if (diff_sec <= 0) {
            fprintf(stderr, "[SPK] 警告: 目标时间已是过去 (差 %ld 秒)，将立即执行\n",
                    diff_sec);
        }
    }

    while (!g_stop_flag) {
        time_t now;
        time(&now);
        long remaining = (long)difftime(target_time, now);
        if (remaining <= 0) break;
        if (remaining <= 10 || remaining % 30 == 0) {
            printf("[SPK] 距离定时播放还有 %ld 秒\n", remaining);
        }
        sleep(1);
    }

    return 0;
}

/**
 * @brief   播放线程主函数
 *
 *          复制请求到本地栈，按 play_type 分支执行：
 *          1=循环  2=顺序  3=定时  4=单次/插播
 */
static void *play_thread_func(void *arg)
{
    (void)arg;

    /* 拷贝请求到栈上，避免全局被意外覆盖 */
    play_request_t req;
    memcpy(&req, &g_request, sizeof(play_request_t));

    printf("[SPK] 播放线程启动: type=%d, index=%d, seq_count=%d\n",
           req.play_type, req.audio_index, req.seq_count);

    switch (req.play_type) {

    case 1: {   /* ---- 循环播放 ---- */
        printf("[SPK] 循环播放: audio=%d\n", req.audio_index);
        while (!g_stop_flag) {
            const char *path = get_audio_path(req.audio_index);
            if (!path) {
                fprintf(stderr, "[SPK] 音频 %d 不存在\n", req.audio_index);
                break;
            }
            time_t t0 = time(NULL);
            play_file_blocking(path);
            if (g_stop_flag) break;

            /*
             * 若子进程在 2 秒内就退出了（异常短），可能是 aplay 启动失败，
             * 额外冷却 3 秒避免日志刷屏和 CPU 空转。
             */
            time_t elapsed = time(NULL) - t0;
            if (elapsed < 2) {
                printf("[SPK] 播放异常短 (%ld 秒)，冷却 3 秒后重试...\n",
                       (long)elapsed);
                for (int cool = 0; cool < 30 && !g_stop_flag; cool++) {
                    usleep(100000);  /* 100ms × 30 = 3 秒 */
                }
            } else {
                usleep(LOOP_GAP_US);
            }
        }
        break;
    }

    case 2: {   /* ---- 顺序播放 ---- */
        printf("[SPK] 顺序播放: %d 个音频\n", req.seq_count);
        for (int i = 0; i < req.seq_count && !g_stop_flag; i++) {
            const char *path = get_audio_path(req.seq[i]);
            if (!path) {
                fprintf(stderr, "[SPK] 音频 %d 不存在，跳过\n", req.seq[i]);
                continue;
            }
            play_file_blocking(path);
            if (g_stop_flag) break;

            if (!g_stop_flag && i < req.seq_count - 1) {
                usleep(SEQ_GAP_US);
            }
        }
        break;
    }

    case 3: {   /* ---- 定时播放 ---- */
        printf("[SPK] 定时播放: time=%s, audio=%d\n",
               req.timed_str, req.audio_index);
        wait_until_time(req.timed_str);
        if (!g_stop_flag) {
            const char *path = get_audio_path(req.audio_index);
            if (path) {
                play_file_blocking(path);
            } else {
                fprintf(stderr, "[SPK] 定时播放失败: 音频 %d 不存在\n",
                        req.audio_index);
            }
        }
        break;
    }

    case 4:     /* ---- 单次 / 插播 ---- */
    default: {
        printf("[SPK] 单次播放: audio=%d\n", req.audio_index);
        const char *path = get_audio_path(req.audio_index);
        if (path) {
            play_file_blocking(path);
        } else {
            fprintf(stderr, "[SPK] 音频 %d 不存在\n", req.audio_index);
        }

        break;
    }

    }

    /* 清理状态 */
    pthread_mutex_lock(&g_spk_mutex);
    g_playing       = 0;
    g_paused        = 0;
    g_current_audio = -1;
    g_player_pid    = -1;
    g_thread_active = 0;
    pthread_mutex_unlock(&g_spk_mutex);
    notify_status_changed();

    printf("[SPK] 播放线程结束\n");
    return NULL;
}

/**
 * @brief   启动新播放（先停止旧播放，再创建新线程）
 */
static int start_playback(const play_request_t *req)
{
    /* 停止旧播放 */
    join_play_thread();

    /*
     * 清理任何残留的孤儿 aplay/mpg123 进程。
     * 旧 sh -c 包装方式可能导致 shell 被杀后 aplay 变成孤儿（PPID=1），
     * 霸占 ALSA 设备导致后续播放报 "Device or resource busy"。
     */
    int ret = system("killall -9 aplay mpg123 2>/dev/null");
    (void)ret;
    usleep(100000);  /* 等 100ms 确保 ALSA 设备释放 */

    g_stop_flag     = 0;
    g_playing        = 0;
    g_paused         = 0;
    g_current_audio  = req->audio_index;
    memcpy(&g_request, req, sizeof(play_request_t));

    g_thread_active = 1;
    if (pthread_create(&g_play_thread, NULL, play_thread_func, NULL) != 0) {
        fprintf(stderr, "[SPK] 创建播放线程失败\n");
        g_thread_active = 0;
        return -1;
    }

    return 0;
}

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

int device_speaker_init(void)
{
    memset(g_audio_list, 0, sizeof(g_audio_list));
    g_playing        = 0;
    g_paused         = 0;
    g_current_audio  = -1;
    g_player_pid     = -1;
    g_thread_active  = 0;
    g_stop_flag      = 0;
    g_status_changed = 0;
    g_status_cb      = NULL;
    g_volume         = DEFAULT_VOLUME;

    ensure_music_dir();
    g_alsa_card = detect_usb_card();
    g_fail_status = (g_alsa_card >= 0) ? 0 : 1;  /* 声卡未检测到则不在线 */
    set_volume(g_volume);

    /* 检查 mpg123 是否可用 */
    if (system("which mpg123 > /dev/null 2>&1") != 0) {
        fprintf(stderr, "[SPK] 警告: mpg123 未安装，MP3 播放将不可用\n");
        fprintf(stderr, "[SPK]   安装: apt install mpg123\n");
    }

    printf("[SPK] 音箱初始化完成 (USB card=%d, 音量=%d%%, 目录=%s)\n",
           g_alsa_card, g_volume, MUSIC_DIR);
    return 0;
}

void device_speaker_set_status_cb(speaker_status_cb_t cb)
{
    g_status_cb = cb;
}

int device_speaker_poll_status_changed(void)
{
    if (g_status_changed) {
        g_status_changed = 0;
        return 1;
    }
    return 0;
}

int device_speaker_execute(const cmd_t *cmd, cJSON **resp)
{
    /* cmd 为空 */
    if (!cmd) {
        fprintf(stderr, "[SPK] cmd 为空\n");
        pthread_mutex_lock(&g_spk_mutex);
        g_fail_status = 2;
        pthread_mutex_unlock(&g_spk_mutex);
        if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, cmd->cmd_id, "cmd 为空");
        return -1;
    }

    printf("[SPK] cmd=%d\n", cmd->cmd_id);

    /* ================================================================ */
    /*  cmd=301: 播放控制                                                */
    /* ================================================================ */
    if (cmd->cmd_id == 301) {

        int play_flag   = cmd_get_int(cmd, "play",   0);
        int resume_flag = cmd_get_int(cmd, "resume", 0);
        int pause_flag  = cmd_get_int(cmd, "pause",  0);
        int stop_flag   = cmd_get_int(cmd, "stop",   0);
        int vol_req     = cmd_get_int(cmd, "volume", 0);
        int play_type   = cmd_get_int(cmd, "playType", 0);
        int audio_index = cmd_get_int(cmd, "audioIndex", -1);

        printf("[SPK] 播放控制: play=%d resume=%d pause=%d stop=%d vol=%d type=%d idx=%d\n",
               play_flag, resume_flag, pause_flag, stop_flag, vol_req, play_type, audio_index);

        /* --- 音量（独立于播放控制，始终处理） --- */
        if (vol_req > 0) {
            set_volume(vol_req);
        }

        /* 仅调节音量（无播放/继续/暂停/停止动作），直接返回成功 */
        if (vol_req > 0 && play_flag == 0 && resume_flag == 0 &&
            pause_flag == 0 && stop_flag == 0) {
            if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "success");
            return 0;
        }

        /* --- 停止 --- */
        if (stop_flag == 1) {
            printf("[SPK] 停止播放\n");
            join_play_thread();
            pthread_mutex_lock(&g_spk_mutex);
            g_playing       = 0;
            g_paused        = 0;
            g_current_audio = -1;
            g_play_type     = 0;
            g_play_data     = 0;
            pthread_mutex_unlock(&g_spk_mutex);
            notify_status_changed();
            if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "success");
            return 0;
        }

        /* --- 暂停 --- */
        if (pause_flag == 1) {
            pthread_mutex_lock(&g_spk_mutex);
            if (!g_playing) {
                pthread_mutex_unlock(&g_spk_mutex);
                if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                     "当前未在播放，无法暂停");
                return -1;
            }
            if (g_paused) {
                pthread_mutex_unlock(&g_spk_mutex);
                if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "已处于暂停状态");
                return 0;
            }
            if (g_player_pid > 0) {
                printf("[SPK] 暂停播放 (pid=%d)\n", g_player_pid);
                kill(g_player_pid, SIGSTOP);
                g_paused = 1;
            }
            pthread_mutex_unlock(&g_spk_mutex);
            notify_status_changed();
            if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "success");
            return 0;
        }

        /* --- 继续播放（仅从暂停处恢复，不启动新任务） --- */
        if (resume_flag == 1) {
            pthread_mutex_lock(&g_spk_mutex);
            if (!g_paused) {
                pthread_mutex_unlock(&g_spk_mutex);
                if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                     "当前无暂停任务，无法继续播放");
                return -1;
            }
            if (g_player_pid > 0) {
                printf("[SPK] 继续播放 (pid=%d)\n", g_player_pid);
                kill(g_player_pid, SIGCONT);
                g_paused = 0;
            }
            pthread_mutex_unlock(&g_spk_mutex);
            notify_status_changed();
            if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "success");
            return 0;
        }

        /* --- 播放（新任务，会顶替掉旧的暂停/播放任务） --- */
        if (play_flag == 1) {

            /* 验证 playType（离散枚举，必须为 1~5） */
            if (play_type < 1 || play_type > 5) {
                if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                     "playType 无效，必须为 1~5");
                return -1;
            }

            /* 构建播放请求 */
            play_request_t req;
            memset(&req, 0, sizeof(req));
            req.play_type   = play_type;
            req.audio_index = audio_index;

            /* V2.7: 追踪播放状态供 get_status 上报 */
            pthread_mutex_lock(&g_spk_mutex);
            g_play_type = req.play_type;
            g_play_data = 0;  /* 先清零，各分支再具体赋值 */
            pthread_mutex_unlock(&g_spk_mutex);

            switch (play_type) {

            case 1: /* 循环播放 */
                /* fall through */
            case 4: /* 插播/单次 */
                if (audio_index < 0 || audio_index >= MAX_AUDIO_FILES) {
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "audioIndex 无效");
                    return -1;
                }
                if (!g_audio_list[audio_index].valid) {
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "音频文件不存在");
                    return -1;
                }
                pthread_mutex_lock(&g_spk_mutex);
                g_play_data = 1;  /* V2.7: 单个音频 */
                pthread_mutex_unlock(&g_spk_mutex);
                break;

            case 2: { /* 顺序播放 */
                cJSON *play_data = cJSON_GetObjectItem(cmd->root, "playData");
                if (!cJSON_IsArray(play_data)) {
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "playData 应为数组");
                    return -1;
                }
                int n = cJSON_GetArraySize(play_data);
                if (n <= 0 || n > MAX_SEQ_LEN) {
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "playData 数量无效");
                    return -1;
                }
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(play_data, i);
                    if (!item) continue;
                    cJSON *j_ai = cJSON_GetObjectItem(item, "audioIndex");
                    int ai = (j_ai && cJSON_IsNumber(j_ai)) ? j_ai->valueint : -1;
                    if (ai < 0 || ai >= MAX_AUDIO_FILES || !g_audio_list[ai].valid) {
                        if (resp) *resp = msg_build_cmd_response(
                            ERR_PARAM_INVALID, 301, "playData 中 audioIndex 无效");
                        return -1;
                    }
                    req.seq[i] = ai;
                }
                req.seq_count = n;
                pthread_mutex_lock(&g_spk_mutex);
                g_play_data = n;  /* V2.7: 顺序播放条数 */
                pthread_mutex_unlock(&g_spk_mutex);
                break;
            }

            case 3: { /* 定时播放 */
                const char *time_str = cmd_get_string(cmd, "playData");
                if (!time_str || strlen(time_str) == 0) {
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "playData 应为时间字符串");
                    return -1;
                }
                memset(req.timed_str, 0, sizeof(req.timed_str));
                strncpy(req.timed_str, time_str, sizeof(req.timed_str) - 1);
                /* 定时播放也需要指定播放哪个音频 */
                if (audio_index < 0 || audio_index >= MAX_AUDIO_FILES ||
                    !g_audio_list[audio_index].valid) {
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "audioIndex 无效");
                    return -1;
                }
                /* 严格校验：定时时间不能为过去 */
                {
                    struct tm target_tm;
                    if (parse_datetime(req.timed_str, &target_tm) != 0) {
                        if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                             "playData 时间格式无效");
                        return -1;
                    }
                    time_t target_time = mktime(&target_tm);
                    if (target_time == (time_t)-1) {
                        if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                             "playData 时间转换失败");
                        return -1;
                    }
                    time_t now;
                    time(&now);
                    if (difftime(target_time, now) <= 0) {
                        fprintf(stderr, "[SPK] 定时时间已是过去: %s"
                                " (当前: %ld, 目标: %ld)\n",
                                req.timed_str, (long)now, (long)target_time);
                        if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                             "定时时间不能为过去的时间");
                        return -1;
                    }
                }
                pthread_mutex_lock(&g_spk_mutex);
                g_play_data = 1;  /* V2.7: 有定时数据 */
                pthread_mutex_unlock(&g_spk_mutex);
                break;
            }

            case 5: {   /* ---- 插播：保存原任务 → 单次播插播 → 等待 → 恢复原任务 ---- */
                if (audio_index < 0 || audio_index >= MAX_AUDIO_FILES ||
                    !g_audio_list[audio_index].valid) {
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "audioIndex 无效");
                    return -1;
                }

                pthread_mutex_lock(&g_spk_mutex);
                if (g_insert_active) {
                    pthread_mutex_unlock(&g_spk_mutex);
                    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                         "已有插播进行中，不支持嵌套插播");
                    return -1;
                }

                int was_playing = (g_playing && g_thread_active);

                if (!was_playing) {
                    /* 无播放任务 → 退化为普通单次播放 */
                    pthread_mutex_unlock(&g_spk_mutex);
                    printf("[SPK] 插播: 当前无播放，作为单次播放\n");
                    play_request_t r;
                    memset(&r, 0, sizeof(r));
                    r.play_type   = 4;
                    r.audio_index = audio_index;
                    if (start_playback(&r) != 0) {
                        if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 301,
                                                             "播放启动失败");
                        return -1;
                    }
                    if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "success");
                    return 0;
                }

                /*
                 * 保存原任务 → 停止原播放 → 播放插播 → 等待插播结束 → 恢复原任务。
                 * 全程使用已有的 join_play_thread/start_playback，不手动 kill，
                 * 不跨线程共享 play_file_blocking，零竞态。
                 */
                play_request_t saved_req;
                memcpy(&saved_req, &g_request, sizeof(play_request_t));

                g_insert_active = 1;
                pthread_mutex_unlock(&g_spk_mutex);

                printf("[SPK] 插播: 保存原任务 (type=%d), 播放 audio=%d\n",
                       saved_req.play_type, audio_index);

                /* 1. 停止原播放（join_play_thread + killall 清理） */
                join_play_thread();

                /* 2. 启动插播（单次播放） */
                {
                    play_request_t ir;
                    memset(&ir, 0, sizeof(ir));
                    ir.play_type   = 4;
                    ir.audio_index = audio_index;
                    start_playback(&ir);
                }

                /* 3. 等待插播结束 */
                while (g_thread_active) {
                    usleep(100000);
                }

                /* 4. 恢复原播放 */
                printf("[SPK] 插播结束，恢复原播放 (type=%d)\n", saved_req.play_type);
                start_playback(&saved_req);

                pthread_mutex_lock(&g_spk_mutex);
                g_insert_active = 0;
                pthread_mutex_unlock(&g_spk_mutex);

                notify_status_changed();
                if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "success");
                return 0;
            }

            default:
                /* playType 已在上面校验为 1~5，不应到达此处 */
                if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                                     "playType 无效");
                return -1;
            }

            /* 启动播放 */
            if (start_playback(&req) != 0) {
                if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 301,
                                                     "播放启动失败");
                return -1;
            }

            if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 301, "success");
            return 0;
        }

        /* 无有效控制标志 */
        if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 301,
                                             "缺少有效控制参数 (play/resume/pause/stop/volume)");
        return -1;
    }

    /* ================================================================ */
    /*  cmd=302: 设置音频文件列表（含下载）                                 */
    /* ================================================================ */
    if (cmd->cmd_id == 302) {

        const char *down_url = cmd_get_string(cmd, "downUrl");
        cJSON *audio_data    = cJSON_GetObjectItem(cmd->root, "audioData");

        if (!down_url || !cJSON_IsArray(audio_data)) {
            if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 302,
                                                  "缺少 downUrl 或 audioData");
            return -1;
        }

        int n = cJSON_GetArraySize(audio_data);
        printf("[SPK] 设置音频列表: %d 个文件, downUrl=%s\n", n, down_url);

        int download_fail = 0;

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(audio_data, i);
            if (!item) continue;

            cJSON *j_idx  = cJSON_GetObjectItem(item, "index");
            cJSON *j_name = cJSON_GetObjectItem(item, "audioName");
            cJSON *j_url  = cJSON_GetObjectItem(item, "url");

            if (!j_idx || !j_name || !j_url) {
                fprintf(stderr, "[SPK] audioData[%d] 字段缺失，跳过\n", i);
                continue;
            }

            int         idx  = j_idx->valueint;
            const char *name = j_name->valuestring;
            const char *url  = j_url->valuestring;

            if (idx < 0 || idx >= MAX_AUDIO_FILES) {
                fprintf(stderr, "[SPK] index %d 超出范围，跳过\n", idx);
                continue;
            }

            /* 拼接下载 URL: downUrl + url */
            char base[512];
            strncpy(base, down_url, sizeof(base) - 1);
            base[sizeof(base) - 1] = '\0';
            size_t blen = strlen(base);
            while (blen > 0 && base[blen - 1] == '/') base[--blen] = '\0';

            char full_url[1024];
            snprintf(full_url, sizeof(full_url), "%s%s", base, url);

            /* 本地路径: MUSIC_DIR + 文件名 */
            const char *filename = strrchr(url, '/');
            if (filename) filename++;
            else filename = url;
            if (*filename == '\0') filename = "unknown.dat";

            char local_path[512];
            snprintf(local_path, sizeof(local_path), "%s%s", MUSIC_DIR, filename);

            /* 存储元数据 */
            g_audio_list[idx].valid = 1;
            g_audio_list[idx].index = idx;
            snprintf(g_audio_list[idx].audio_name,
             sizeof(g_audio_list[idx].audio_name),
             "%s", name);
            snprintf(g_audio_list[idx].local_path,
             sizeof(g_audio_list[idx].local_path),
             "%s", local_path);

            /* 如果文件已存在则跳过下载 */
            if (access(local_path, F_OK) == 0) {
                printf("[SPK] [%d] 文件已存在: %s\n", idx, local_path);
                continue;
            }

            /* wget 下载 */
            char wget_cmd[2048];
            snprintf(wget_cmd, sizeof(wget_cmd),
                     "wget -q -O '%s' '%s' 2>/dev/null",
                     local_path, full_url);

            printf("[SPK] [%d] 下载: %s -> %s\n", idx, full_url, local_path);
            int ret = system(wget_cmd);
            if (ret != 0) {
                fprintf(stderr, "[SPK] [%d] 下载失败: %s\n", idx, full_url);
                download_fail++;
            }
        }

        if (resp) {
            if (download_fail > 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "%d 个文件下载失败", download_fail);
                *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 302, msg);
            } else {
                *resp = msg_build_cmd_response(ERR_SUCCESS, 302, "success");
            }
        }
        return (download_fail > 0) ? -1 : 0;
    }

    /* ================================================================ */
    /*  cmd=303: 获取音频文件列表                                          */
    /* ================================================================ */
    if (cmd->cmd_id == 303) {

        printf("[SPK] 获取音频列表\n");

        if (resp) {
            /* 统计有效音频数量 + 构建 audioData 数组 */
            int audio_count = 0;
            cJSON *arr = cJSON_CreateArray();
            for (int i = 0; i < MAX_AUDIO_FILES; i++) {
                if (g_audio_list[i].valid) {
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddNumberToObject(item, "index",
                                            g_audio_list[i].index);
                    cJSON_AddStringToObject(item, "audioName",
                                            g_audio_list[i].audio_name);
                    cJSON_AddItemToArray(arr, item);
                    audio_count++;
                }
            }
            *resp = msg_build_spk_getlist_response(ERR_SUCCESS, "获取成功",
                                                    audio_count, arr);
            cJSON_Delete(arr);
        }
        return 0;
    }

    /* ---- 未知 cmd ---- */
    fprintf(stderr, "[SPK] 未知 cmd: %d\n", cmd->cmd_id);
    if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, cmd->cmd_id, "无效的 cmd");
    return -1;
}

cJSON *device_speaker_get_status(void)
{
    /*
     * V3.1: 使用 trylock 防阻塞。
     * 心跳和 cmd=601 在主线程调用，speaker worker/playback 线程
     * 可能短暂持有 g_spk_mutex。trylock 失败时无锁读取。
     */
    int vol, idx, pt, pd, fs;

    if (pthread_mutex_trylock(&g_spk_mutex) == 0) {
        vol = g_volume;
        idx = g_current_audio;
        pt  = g_play_type;
        pd  = g_play_data;
        fs  = g_fail_status;
        pthread_mutex_unlock(&g_spk_mutex);
    } else {
        vol = g_volume;
        idx = g_current_audio;
        pt  = g_play_type;
        pd  = g_play_data;
        fs  = g_fail_status;
    }

    /* 硬件存活检测：USB 声卡不存在则判定离线 */
    bool hw_ok = (g_alsa_card >= 0);

    cJSON *status = cJSON_CreateObject();
    cJSON_AddBoolToObject  (status, "online",     (hw_ok && fs == 0));
    cJSON_AddNumberToObject(status, "volume",     vol);
    cJSON_AddNumberToObject(status, "audioIndex", idx >= 0 ? idx : 0);
    cJSON_AddNumberToObject(status, "playType",   pt);
    cJSON_AddNumberToObject(status, "playData",   pd);

    return status;
}

int device_speaker_get_fail_status(void)
{
    return g_fail_status;
}

void device_speaker_apply_config(int volume)
{
    pthread_mutex_lock(&g_spk_mutex);
    set_volume(volume);
    pthread_mutex_unlock(&g_spk_mutex);

    printf("[SPK] 配置已恢复: volume=%d\n", volume);
}

void device_speaker_deinit(void)
{
    join_play_thread();
    kill_player();
    g_playing        = 0;
    g_paused         = 0;
    g_current_audio  = -1;
    g_play_type      = 0;
    g_play_data      = 0;
    g_status_changed = 0;
    g_status_cb      = NULL;
    printf("[SPK] 音箱模块已释放\n");
}