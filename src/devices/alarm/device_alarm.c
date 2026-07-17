/**
 * @file    device_alarm.c
 * @brief   警灯控制实现（V2.9 — 平台 GPIO 适配）
 *
 *          GPIO4 控制路径: /sys/devices/platform/gpio_out/gpio_out4/value
 *            模式切换: 高 0.5s → 低 0.5s 为一次，需切换 |target - current| 次
 *          GPIO3 控制路径: /sys/devices/platform/gpio_out/gpio_out3/value
 *            警灯电源开关: 高电平=开, 低电平=关, 默认开
 */

#define _GNU_SOURCE

#include "device_alarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#include "msg_builder.h"
#include "config.h"

/* ======================================================================== */
/*  常量定义                                                                  */
/* ======================================================================== */

/*
 * 平台 GPIO 路径，对应 J_I2C 座子 2脚 → gpio_out4
 *   座子引脚映射：
 *     5脚 → gpio_out1
 *     4脚 → gpio_out2
 *     3脚 → gpio_out3
 *     2脚 → gpio_out4  ← 当前使用
 */
#define GPIO_VALUE_PATH       "/sys/devices/platform/gpio_out/gpio_out4"
#define GPIO3_VALUE_PATH      "/sys/devices/platform/gpio_out/gpio_out3"
#define SWITCH_DELAY_US       1000000
#define MODE_MIN              0
#define MODE_MAX              14
#define MODE_DEFAULT          0

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
/*  模块内部状态                                                               */
/* ======================================================================== */

static int           g_current_mode      = MODE_DEFAULT;
static int           g_alarm_on          = 0;
static const char   *g_gpio_chip         = NULL;
static int           g_gpio_pin          = -1;
static pthread_t     g_timer_thread;
static volatile int  g_timer_running     = 0;
static int           g_thread_active     = 0;

typedef void (*alarm_status_cb_t)(void);
static alarm_status_cb_t g_status_changed_cb = NULL;

static volatile int  g_status_changed    = 0;
static int           g_fail_status      = 0;       /* 0=正常, 1=不在线, 2=响应异常 */

/* 互斥锁，保护 GPIO 操作和状态变量的并发读写（worker 线程 + 定时线程 + 心跳线程） */
static pthread_mutex_t g_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ======================================================================== */
/*  底层 GPIO 操作                                                             */
/* ======================================================================== */

/**
 * @brief   往 GPIO value 文件写入 0 或 1
 * @param   value   0=低电平, 1=高电平
 * @return  0 成功，-1 失败
 */
static int gpio_write(int value)
{
    FILE *fp = fopen(GPIO_VALUE_PATH, "w");
    if (!fp) {
        fprintf(stderr, "[ALARM] GPIO 写入失败: %s: %s\n",
                GPIO_VALUE_PATH, strerror(errno));
        return -1;
    }
    fprintf(fp, "%d", value);
    fclose(fp);
    return 0;
}

/**
 * @brief   往 GPIO3 value 文件写入 0 或 1（警灯电源开关）
 * @param   value   0=低电平(关), 1=高电平(开)
 * @return  0 成功，-1 失败
 */
static int gpio3_write(int value)
{
    FILE *fp = fopen(GPIO3_VALUE_PATH, "w");
    if (!fp) {
        fprintf(stderr, "[ALARM] GPIO3 写入失败: %s: %s\n",
                GPIO3_VALUE_PATH, strerror(errno));
        return -1;
    }
    fprintf(fp, "%d", value);
    fclose(fp);
    return 0;
}

static int do_one_switch(void)
{
    if (gpio_write(1) != 0) return -1;
    usleep(SWITCH_DELAY_US);
    if (gpio_write(0) != 0) return -1;
    usleep(SWITCH_DELAY_US);
    return 0;
}

static void gpio_off(void) { gpio_write(0); }

/* ======================================================================== */
/*  模式切换核心逻辑                                                           */
/* ======================================================================== */

static int switch_to_mode(int target_mode)
{
    if (target_mode < MODE_MIN || target_mode > MODE_MAX) {
        fprintf(stderr, "[ALARM] 无效模式 %d (有效 %d~%d)\n",
                target_mode, MODE_MIN, MODE_MAX);
        return -1;
    }

    int diff  = target_mode - g_current_mode;
    int steps = abs(diff);

    if (steps == 0) {
        printf("[ALARM] 已在模式 %d，无需切换\n", target_mode);
        return 0;
    }

    printf("[ALARM] 切换模式: %d → %d (%d 次)\n",
           g_current_mode, target_mode, steps);

    int direction = (diff > 0) ? 1 : -1;
    for (int i = 0; i < steps; i++) {
        printf("[ALARM]   切换 %d/%d: %d → %d\n",
               i + 1, steps, g_current_mode, g_current_mode + direction);
        if (do_one_switch() != 0) {
            fprintf(stderr, "[ALARM] 切换失败 (第 %d 次)\n", i + 1);
            return -1;
        }
        g_current_mode += direction;
    }

    printf("[ALARM] 切换完成，当前模式: %d, GPIO 低电平\n", g_current_mode);
    return 0;
}

/* ======================================================================== */
/*  定时开关线程                                                               */
/* ======================================================================== */

typedef struct {
    int   light_type;
    char  open_data[32];
} alarm_timer_arg_t;

static void join_old_thread(void)
{
    if (g_thread_active) {
        pthread_join(g_timer_thread, NULL);
        g_thread_active = 0;
    }
}

static int parse_datetime(const char *str, struct tm *out_tm)
{
    if (!str || !out_tm) return -1;

    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

    if (sscanf(str, "%d-%d-%d %d:%d:%d",
               &year, &month, &day, &hour, &min, &sec) != 6) {
        fprintf(stderr, "[ALARM] 时间格式解析失败（字段不足 6 个）: %s\n", str);
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

static void notify_status_changed(void)
{
    g_status_changed = 1;
    if (g_status_changed_cb) {
        g_status_changed_cb();
    }
}

static void *alarm_timer_thread(void *arg)
{
    alarm_timer_arg_t *timer_arg = (alarm_timer_arg_t *)arg;
    if (!timer_arg) {
        g_timer_running = 0;
        return NULL;
    }

    printf("[ALARM] 定时线程启动，目标时间: %s, 模式: %d\n",
           timer_arg->open_data, timer_arg->light_type);

    struct tm target_tm;
    if (parse_datetime(timer_arg->open_data, &target_tm) != 0) {
        fprintf(stderr, "[ALARM] 定时时间解析失败，放弃执行\n");
        free(timer_arg);
        g_timer_running = 0;
        return NULL;
    }

    time_t target_time = mktime(&target_tm);
    if (target_time == (time_t)-1) {
        fprintf(stderr, "[ALARM] mktime 转换失败，输入: %s\n",
                timer_arg->open_data);
        free(timer_arg);
        g_timer_running = 0;
        return NULL;
    }

    time_t now;
    time(&now);
    long diff_sec = (long)difftime(target_time, now);
    printf("[ALARM] 目标 epoch: %ld, 当前 epoch: %ld, 差值: %ld 秒\n",
           (long)target_time, (long)now, diff_sec);

    /* 防御性检查：正常情况下 cmd 层已拦截过期时间，此处作为最后防线 */
    if (diff_sec <= 0) {
        fprintf(stderr, "[ALARM] 警告: 目标时间已是过去 (差 %ld 秒)，"
                "将立即执行\n", diff_sec);
    }

    while (g_timer_running) {
        time(&now);
        long remaining = (long)difftime(target_time, now);
        if (remaining <= 0) break;
        if (remaining <= 10 || remaining % 30 == 0) {
            printf("[ALARM]   距离目标还有 %ld 秒\n", remaining);
        }
        sleep(1);
    }

    if (g_timer_running) {
        printf("[ALARM] 定时到达，开启警灯，模式: %d\n", timer_arg->light_type);
        pthread_mutex_lock(&g_alarm_mutex);
        gpio3_write(1);  /* GPIO3 高电平，打开警灯电源 */
        g_alarm_on = 1;
        switch_to_mode(timer_arg->light_type);
        pthread_mutex_unlock(&g_alarm_mutex);
        notify_status_changed();
    } else {
        printf("[ALARM] 定时任务被取消\n");
    }

    free(timer_arg);
    g_timer_running = 0;
    return NULL;
}

static int start_alarm_timer(int light_type, const char *open_data)
{
    join_old_thread();

    alarm_timer_arg_t *arg = (alarm_timer_arg_t *)malloc(sizeof(alarm_timer_arg_t));
    if (!arg) return -1;

    arg->light_type = light_type;
    memset(arg->open_data, 0, sizeof(arg->open_data));
    strncpy(arg->open_data, open_data, sizeof(arg->open_data) - 1);

    g_timer_running = 1;
    if (pthread_create(&g_timer_thread, NULL, alarm_timer_thread, arg) != 0) {
        fprintf(stderr, "[ALARM] 创建定时线程失败\n");
        free(arg);
        g_timer_running = 0;
        return -1;
    }
    g_thread_active = 1;

    return 0;
}

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

int device_alarm_init(const char *gpio_chip, int gpio_pin)
{
    g_gpio_chip      = gpio_chip;
    g_gpio_pin       = gpio_pin;
    g_current_mode   = MODE_DEFAULT;
    g_alarm_on       = 0;
    g_timer_running  = 0;
    g_thread_active  = 0;
    g_status_changed = 0;
    g_status_changed_cb = NULL;

    if (access(GPIO_VALUE_PATH, W_OK) != 0) {
        fprintf(stderr, "[ALARM] 警告: %s 不可写: %s\n",
                GPIO_VALUE_PATH, strerror(errno));
        g_fail_status = 1;  /* 不在线 */
    } else {
        g_fail_status = 0;
    }

    /* 检查 GPIO3（电源开关）可写性 */
    if (access(GPIO3_VALUE_PATH, W_OK) != 0) {
        fprintf(stderr, "[ALARM] 警告: %s 不可写: %s\n",
                GPIO3_VALUE_PATH, strerror(errno));
    }

    gpio_off();
    gpio3_write(1);  /* GPIO3 默认高电平（开） */
    printf("[ALARM] 初始化完成 (chip=%s pin=%d, gpio=%s, gpio3=%s), 当前模式: %d, 电源: 开\n",
           gpio_chip ? gpio_chip : "N/A", g_gpio_pin,
           GPIO_VALUE_PATH, GPIO3_VALUE_PATH, g_current_mode);
    return 0;
}

void device_alarm_set_status_cb(alarm_status_cb_t cb)
{
    g_status_changed_cb = cb;
}

int device_alarm_poll_status_changed(void)
{
    if (g_status_changed) {
        g_status_changed = 0;
        return 1;
    }
    return 0;
}

int device_alarm_execute(const cmd_t *cmd, cJSON **resp)
{
    if (!cmd) {
        fprintf(stderr, "[ALARM] cmd 为空\n");
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "cmd 为空");
        return -1;
    }

    printf("[ALARM] cmd=%d\n", cmd->cmd_id);

    if (cmd->cmd_id != CMD_ALARM_CTRL) {
        fprintf(stderr, "[ALARM] 无效 cmd: %d\n", cmd->cmd_id);
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "无效的 cmd");
        return -1;
    }

    int         state      = cmd_get_int(cmd, "state", -1);
    int         light_type = cmd_get_int(cmd, "lightType", MODE_DEFAULT);
    int         open_type  = cmd_get_int(cmd, "openType", 0);
    const char *open_data  = cmd_get_string(cmd, "openData");

    if (state < 0 || state > 1) {
        fprintf(stderr, "[ALARM] state 无效: %d\n", state);
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "state 无效，必须为 0 或 1");
        return -1;
    }

    if (light_type < MODE_MIN || light_type > MODE_MAX) {
        fprintf(stderr, "[ALARM] lightType 超出范围: %d\n", light_type);
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "lightType 超出范围");
        return -1;
    }

    if (open_type < 0 || open_type > 1) {
        fprintf(stderr, "[ALARM] openType 无效: %d\n", open_type);
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "openType 无效，必须为 0 或 1");
        return -1;
    }

    /* ---- 关闭警灯 ---- */
    if (state == 0) {
        printf("[ALARM] 关闭警灯\n");
        if (g_timer_running) {
            g_timer_running = 0;
            join_old_thread();
        }
        pthread_mutex_lock(&g_alarm_mutex);
        gpio_off();
        gpio3_write(0);  /* GPIO3 低电平，关闭警灯电源 */
        g_alarm_on = 0;
        g_fail_status = 0;
        pthread_mutex_unlock(&g_alarm_mutex);
        notify_status_changed();
        if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
        return 0;
    }

    /* ---- 打开警灯 ---- */
    if (state == 1) {
        if (open_type == 1) {
            if (!open_data || strlen(open_data) == 0) {
                if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                     "openType=1 时 openData 不能为空");
                return -1;
            }
            /* 严格校验：定时时间不能为过去 */
            {
                struct tm target_tm;
                if (parse_datetime(open_data, &target_tm) != 0) {
                    if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                         "openData 时间格式无效");
                    return -1;
                }
                time_t target_time = mktime(&target_tm);
                if (target_time == (time_t)-1) {
                    if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                         "openData 时间转换失败");
                    return -1;
                }
                time_t now;
                time(&now);
                if (difftime(target_time, now) <= 0) {
                    fprintf(stderr, "[ALARM] 定时时间已是过去: %s"
                            " (当前: %ld, 目标: %ld)\n",
                            open_data, (long)now, (long)target_time);
                    if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                         "定时时间不能为过去的时间");
                    return -1;
                }
            }
            printf("[ALARM] 定时开启，模式: %d, 时间: %s\n",
                   light_type, open_data);
            if (start_alarm_timer(light_type, open_data) != 0) {
                if (resp) *resp = msg_build_response(ERR_DEVICE_COMM,
                                                     "定时任务启动失败");
                return -1;
            }
            g_fail_status = 0;  /* 定时任务启动成功，恢复正常 */
            if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
            return 0;
        }

        printf("[ALARM] 正常开启，目标模式: %d\n", light_type);
        pthread_mutex_lock(&g_alarm_mutex);
        gpio3_write(1);  /* GPIO3 高电平，打开警灯电源 */
        if (switch_to_mode(light_type) != 0) {
            g_fail_status = 2;  /* 响应异常 */
            pthread_mutex_unlock(&g_alarm_mutex);
            if (resp) *resp = msg_build_response(ERR_DEVICE_COMM, "模式切换失败");
            return -1;
        }
        g_alarm_on = 1;
        g_fail_status = 0;  /* 恢复正常 */
        pthread_mutex_unlock(&g_alarm_mutex);
        notify_status_changed();
        if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
        return 0;
    }

    fprintf(stderr, "[ALARM] 未知 state: %d\n", state);
    if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "state 值无效");
    return -1;
}

cJSON *device_alarm_get_status(void)
{
    /*
     * V3.1: 使用 trylock 防阻塞。
     * alarm worker 在 switch_to_mode() 期间持有 g_alarm_mutex
     * 最长可达 28s（14 步 × 2s）。心跳和 cmd=601 不能等。
     */
    int on, mode, fs;

    if (pthread_mutex_trylock(&g_alarm_mutex) == 0) {
        on   = g_alarm_on;
        mode = g_current_mode;
        fs   = g_fail_status;
        pthread_mutex_unlock(&g_alarm_mutex);
    } else {
        on   = g_alarm_on;
        mode = g_current_mode;
        fs   = g_fail_status;
    }

    /* 硬件存活检测：GPIO 路径不可写则判定离线 */
    bool hw_ok = (access(GPIO_VALUE_PATH, W_OK) == 0);

    cJSON *status = cJSON_CreateObject();
    cJSON_AddBoolToObject  (status, "online",    (hw_ok && fs == 0));
    cJSON_AddNumberToObject(status, "state",     on);
    cJSON_AddNumberToObject(status, "lightType", mode);

    return status;
}

int device_alarm_get_fail_status(void)
{
    /* int 读取单指令，无需加锁 */
    return g_fail_status;
}

void device_alarm_deinit(void)
{
    if (g_timer_running) {
        g_timer_running = 0;
    }
    join_old_thread();
    gpio_off();
    gpio3_write(1);     /* GPIO3 恢复默认高电平（开） */
    g_alarm_on          = 0;
    g_current_mode      = MODE_DEFAULT;
    g_status_changed    = 0;
    g_status_changed_cb = NULL;
    printf("[ALARM] 警灯模块已释放\n");
}