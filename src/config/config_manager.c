/**
 * @file    config_manager.c
 * @brief   运行时配置管理器实现
 *
 *          7 个类别独立 JSON 文件，原子写入，启动加载/默认值兜底。
 */

#include "config_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include "config.h"
#include "device_ptz.h"
#include "device_led.h"
#include "device_speaker.h"
#include "device_alarm.h"
#include "device_mood.h"
#include "preset_config.h"

/* ======================================================================== */
/*  内部常量                                                                  */
/* ======================================================================== */

enum {
    CAT_SERVER  = 0,
    CAT_PTZ     = 1,
    CAT_LED     = 2,
    CAT_SPEAKER = 3,
    CAT_ALARM   = 4,
    CAT_MOOD    = 5,
    CAT_SYSTEM  = 6,
    CAT_COUNT   = 7,
};

static const char *g_category_names[CAT_COUNT] = {
    "server", "ptz", "led", "speaker", "alarm", "mood", "system"
};

/* ======================================================================== */
/*  全局状态                                                                  */
/* ======================================================================== */

static char    g_config_dir[256] = {0};
static cJSON  *g_cache[CAT_COUNT] = {0};
static bool    g_inited = false;

/* 互斥锁，保护 g_cache[] 和文件写入的并发访问（多个 worker 线程 + 心跳线程） */
static pthread_mutex_t g_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ======================================================================== */
/*  内部工具函数                                                               */
/* ======================================================================== */

/** 构建 category 对应的完整文件路径，如 "./config/ptz.json" */
static void make_path(const char *category, char *buf, size_t size)
{
    snprintf(buf, size, "%s/%s.json", g_config_dir, category);
}

/** 原子写入：先写 .tmp 再 rename */
static int save_atomic(const char *category, cJSON *json)
{
    char path[512], tmp[520];
    make_path(category, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    char *str = cJSON_Print(json);
    if (!str) {
        printf("[CFG] JSON 序列化失败: %s\n", category);
        return -1;
    }

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        printf("[CFG] 无法写入 %s: %s\n", tmp, strerror(errno));
        free(str);
        return -1;
    }

    fprintf(fp, "%s\n", str);
    fclose(fp);
    free(str);

    if (rename(tmp, path) != 0) {
        printf("[CFG] rename 失败 %s → %s: %s\n", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    return 0;
}

/** 用 status JSON 中的字段覆盖更新 cache 中的对应 key（浅合并） */
static void merge_status(cJSON *cache, cJSON *status)
{
    if (!cache || !status) return;

    cJSON *item = status->child;
    while (item) {
        cJSON *old = cJSON_DetachItemFromObject(cache, item->string);
        if (old) cJSON_Delete(old);
        cJSON_AddItemToObject(cache, item->string, cJSON_Duplicate(item, 1));
        item = item->next;
    }
}

/** 更新 cache 中的 online 和 failStatus（基于设备 fail_status 值） */
static void update_fail_status(cJSON *cache, int fail_status)
{
    if (!cache) return;

    cJSON *old_on = cJSON_DetachItemFromObject(cache, "online");
    if (old_on) cJSON_Delete(old_on);
    cJSON_AddBoolToObject(cache, "online", (fail_status == 0));

    cJSON *old_fs = cJSON_DetachItemFromObject(cache, "failStatus");
    if (old_fs) cJSON_Delete(old_fs);
    cJSON_AddNumberToObject(cache, "failStatus", fail_status);
}

/* ======================================================================== */
/*  默认值构建（config.h 宏作为兜底）                                           */
/* ======================================================================== */

static cJSON *build_default_server(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "serverUrl",         WS_SERVER_URL);
    cJSON_AddStringToObject(j, "authToken",         "");
    cJSON_AddNumberToObject(j, "reconnectInterval", WS_RECONNECT_INTERVAL);
    cJSON_AddNumberToObject(j, "pingInterval",      WS_PING_INTERVAL);
    cJSON_AddBoolToObject  (j, "connected",         false);
    return j;
}

static cJSON *build_default_ptz(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (j, "online",           true);
    cJSON_AddNumberToObject(j, "failStatus",       0);
    cJSON_AddNumberToObject(j, "horizontalAngle",   180);
    cJSON_AddNumberToObject(j, "verticalAngle",     0);
    cJSON_AddNumberToObject(j, "speed",             0);
    cJSON_AddStringToObject(j, "uartDev",           PTZ_UART_DEV);
    cJSON_AddNumberToObject(j, "baudrate",          PTZ_UART_BAUDRATE);
    return j;
}

static cJSON *build_default_led(void)
{
    cJSON *j = cJSON_CreateObject();

    cJSON *arr = cJSON_CreateArray();
    cJSON *item0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(item0, "textindex", 0);
    cJSON_AddStringToObject(item0, "text",      "");
    cJSON_AddItemToArray(arr, item0);

    cJSON_AddBoolToObject  (j, "online",        true);
    cJSON_AddNumberToObject(j, "failStatus",    0);
    cJSON_AddItemToObject  (j, "textData",      arr);
    cJSON_AddNumberToObject(j, "lightVal",      5);
    cJSON_AddNumberToObject(j, "showType",      0);
    cJSON_AddNumberToObject(j, "displayStyle",  0);
    cJSON_AddStringToObject(j, "netIp",         LED_NET_IP);
    cJSON_AddNumberToObject(j, "netPort",       LED_NET_PORT);
    return j;
}

static cJSON *build_default_speaker(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (j, "online",     true);
    cJSON_AddNumberToObject(j, "failStatus", 0);
    cJSON_AddNumberToObject(j, "volume",     80);
    cJSON_AddNumberToObject(j, "audioIndex", 0);
    cJSON_AddNumberToObject(j, "playType",   0);
    cJSON_AddNumberToObject(j, "playData",   0);
    return j;
}

static cJSON *build_default_alarm(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (j, "online",     true);
    cJSON_AddNumberToObject(j, "failStatus", 0);
    cJSON_AddNumberToObject(j, "state",      0);
    cJSON_AddNumberToObject(j, "lightType",  0);
    cJSON_AddStringToObject(j, "gpioChip",   ALARM_GPIO_CHIP);
    cJSON_AddNumberToObject(j, "gpioPin",    ALARM_GPIO_PIN);
    return j;
}

static cJSON *build_default_mood(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (j, "online", true);
    cJSON_AddStringToObject(j, "state",  "off");
    cJSON_AddStringToObject(j, "type",   "steady");
    return j;
}

static cJSON *build_default_system(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "model",             CTRL_BOARD_MODEL);
    cJSON_AddStringToObject(j, "fwVersion",         CTRL_BOARD_FW_VERSION);
    cJSON_AddStringToObject(j, "mac",               DEVICE_MAC);
    cJSON_AddNumberToObject(j, "heartbeatInterval", HEARTBEAT_INTERVAL);
    return j;
}

/** 构建默认 JSON 的分发表 */
typedef cJSON *(*default_builder_t)(void);

static const default_builder_t g_default_builders[CAT_COUNT] = {
    build_default_server,
    build_default_ptz,
    build_default_led,
    build_default_speaker,
    build_default_alarm,
    build_default_mood,
    build_default_system,
};

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

int config_manager_init(const char *config_dir)
{
    if (g_inited) {
        printf("[CFG] 已初始化，跳过\n");
        return 0;
    }

    /* 保存路径 */
    strncpy(g_config_dir, config_dir, sizeof(g_config_dir) - 1);
    g_config_dir[sizeof(g_config_dir) - 1] = '\0';

    /* 创建目录 */
    if (mkdir(g_config_dir, 0755) != 0 && errno != EEXIST) {
        printf("[CFG] 创建目录失败 %s: %s\n", g_config_dir, strerror(errno));
        return -1;
    }
    printf("[CFG] 配置目录: %s\n", g_config_dir);

    /* 逐类别加载或创建默认 */
    for (int i = 0; i < CAT_COUNT; i++) {
        char path[512];
        make_path(g_category_names[i], path, sizeof(path));

        /* 尝试读取已有文件 */
        FILE *fp = fopen(path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            rewind(fp);

            if (sz > 0 && sz < 65536) {  /* 最大 64KB */
                char *buf = (char *)malloc(sz + 1);
                if (buf) {
                    size_t n = fread(buf, 1, sz, fp);
                    buf[n] = '\0';
                    g_cache[i] = cJSON_Parse(buf);
                    free(buf);
                    if (g_cache[i]) {
                        printf("[CFG] 加载: %s\n", path);
                    }
                }
            }
            fclose(fp);
        }

        /* 解析失败或文件不存在则使用默认值并保存 */
        if (!g_cache[i]) {
            g_cache[i] = g_default_builders[i]();
            if (g_cache[i]) {
                save_atomic(g_category_names[i], g_cache[i]);
                printf("[CFG] 新建默认: %s\n", path);
            }
        }
    }

    g_inited = true;
    printf("[CFG] 配置管理器初始化完成 (%d 个类别)\n", CAT_COUNT);
    return 0;
}

void config_manager_restore_device_state(void)
{
    if (!g_inited) return;

    pthread_mutex_lock(&g_cfg_mutex);

    /* ---- LED: 恢复 textData[0].text, lightVal, showType, displayStyle ---- */
    if (g_cache[CAT_LED]) {
        const char *text = NULL;
        cJSON *text_data = cJSON_GetObjectItem(g_cache[CAT_LED], "textData");
        if (cJSON_IsArray(text_data)) {
            cJSON *first = cJSON_GetArrayItem(text_data, 0);
            if (first) {
                cJSON *t = cJSON_GetObjectItem(first, "text");
                if (cJSON_IsString(t)) {
                    text = t->valuestring;
                }
            }
        }

        int light_val     = 5;
        int show_type     = 0;
        int display_style = 0;

        cJSON *j_light = cJSON_GetObjectItem(g_cache[CAT_LED], "lightVal");
        if (cJSON_IsNumber(j_light)) light_val = j_light->valueint;

        cJSON *j_show = cJSON_GetObjectItem(g_cache[CAT_LED], "showType");
        if (cJSON_IsNumber(j_show)) show_type = j_show->valueint;

        cJSON *j_disp = cJSON_GetObjectItem(g_cache[CAT_LED], "displayStyle");
        if (cJSON_IsNumber(j_disp)) display_style = j_disp->valueint;

        device_led_apply_config(text, light_val, show_type, display_style);
        printf("[CFG] LED 配置已从文件恢复到模块\n");
    }

    /* ---- 音箱: 恢复 volume ---- */
    if (g_cache[CAT_SPEAKER]) {
        int volume = 80;
        cJSON *j_vol = cJSON_GetObjectItem(g_cache[CAT_SPEAKER], "volume");
        if (cJSON_IsNumber(j_vol)) {
            volume = j_vol->valueint;
            if (volume < 0)   volume = 0;
            if (volume > 100) volume = 100;
        }
        device_speaker_apply_config(volume);
        printf("[CFG] 音箱配置已从文件恢复到模块\n");
    }

    /* ---- 氛围灯: 恢复 state 和 type ---- */
    if (g_cache[CAT_MOOD]) {
        const char *state = NULL;
        const char *type  = NULL;

        cJSON *j_state = cJSON_GetObjectItem(g_cache[CAT_MOOD], "state");
        if (cJSON_IsString(j_state)) state = j_state->valuestring;

        cJSON *j_type = cJSON_GetObjectItem(g_cache[CAT_MOOD], "type");
        if (cJSON_IsString(j_type)) type = j_type->valuestring;

        device_mood_apply_config(state ? state : "off",
                                 type  ? type  : "steady");
        printf("[CFG] 氛围灯配置已从文件恢复到模块\n");
    }

    /*
     * PTZ:  跳过 — 角度由硬件传感器实时决定，文件值是旧快照
     * 警灯: 跳过 — 重启后应为关闭状态，安全原因
     */

    pthread_mutex_unlock(&g_cfg_mutex);

    printf("[CFG] 设备状态已从配置文件恢复到各模块\n");
}

void config_manager_sync_device_status(void)
{
    if (!g_inited) return;

    pthread_mutex_lock(&g_cfg_mutex);

    /* ---- ptz ---- */
    cJSON *ptz = device_ptz_get_status();
    if (ptz) {
        merge_status(g_cache[CAT_PTZ], ptz);
        update_fail_status(g_cache[CAT_PTZ], device_ptz_get_fail_status());
        save_atomic("ptz", g_cache[CAT_PTZ]);
        cJSON_Delete(ptz);
    }

    /* ---- led ---- */
    cJSON *led = device_led_get_status();
    if (led) {
        merge_status(g_cache[CAT_LED], led);
        update_fail_status(g_cache[CAT_LED], device_led_get_fail_status());
        save_atomic("led", g_cache[CAT_LED]);
        cJSON_Delete(led);
    }

    /* ---- speaker ---- */
    cJSON *speaker = device_speaker_get_status();
    if (speaker) {
        merge_status(g_cache[CAT_SPEAKER], speaker);
        update_fail_status(g_cache[CAT_SPEAKER], device_speaker_get_fail_status());
        save_atomic("speaker", g_cache[CAT_SPEAKER]);
        cJSON_Delete(speaker);
    }

    /* ---- alarm ---- */
    cJSON *alarm = device_alarm_get_status();
    if (alarm) {
        merge_status(g_cache[CAT_ALARM], alarm);
        update_fail_status(g_cache[CAT_ALARM], device_alarm_get_fail_status());
        save_atomic("alarm", g_cache[CAT_ALARM]);
        cJSON_Delete(alarm);
    }

    /* ---- mood ---- */
    cJSON *mood = device_mood_get_status();
    if (mood) {
        merge_status(g_cache[CAT_MOOD], mood);
        save_atomic("mood", g_cache[CAT_MOOD]);
        cJSON_Delete(mood);
    }

    printf("[CFG] 设备状态已同步到配置文件\n");

    pthread_mutex_unlock(&g_cfg_mutex);
}

void config_manager_update_after_cmd(int cmd_id)
{
    if (!g_inited) return;

    pthread_mutex_lock(&g_cfg_mutex);

    switch (cmd_id) {

    /* ---- 云台 cmd=101~106 ---- */
    case CMD_PTZ_MOVE:       /* 101 */
    case CMD_PTZ_HOME:       /* 102 */
    case CMD_PTZ_CRUISE:     /* 104 */
    case CMD_PTZ_SCAN:       /* 105 */
    case CMD_PTZ_PRESET_QRY: /* 106 */
        {
            cJSON *ptz = device_ptz_get_status();
            if (ptz) {
                merge_status(g_cache[CAT_PTZ], ptz);
                save_atomic("ptz", g_cache[CAT_PTZ]);
                cJSON_Delete(ptz);
            }
        }
        break;

    case CMD_PTZ_PRESET: /* 103 — 额外保存预置位组 YAML */
        {
            cJSON *ptz = device_ptz_get_status();
            if (ptz) {
                merge_status(g_cache[CAT_PTZ], ptz);
                cJSON_Delete(ptz);
            }
            /* 保存预置位数据到 preset_positions.yaml */
            config_manager_save_presets();
        }
        break;

    /* ---- LED cmd=201~202 ---- */
    case CMD_LED_TEXT:   /* 201 */
    case CMD_LED_SWITCH: /* 202 */
        {
            cJSON *led = device_led_get_status();
            if (led) {
                merge_status(g_cache[CAT_LED], led);
                save_atomic("led", g_cache[CAT_LED]);
                cJSON_Delete(led);
            }
        }
        break;

    /* ---- 音箱 cmd=301~303 ---- */
    case CMD_SPK_PLAY:    /* 301 */
    case CMD_SPK_SETLIST: /* 302 */
    case CMD_SPK_GETLIST: /* 303 */
        {
            cJSON *speaker = device_speaker_get_status();
            if (speaker) {
                merge_status(g_cache[CAT_SPEAKER], speaker);
                save_atomic("speaker", g_cache[CAT_SPEAKER]);
                cJSON_Delete(speaker);
            }
        }
        break;

    /* ---- 警灯 cmd=401 ---- */
    case CMD_ALARM_CTRL: /* 401 */
        {
            cJSON *alarm = device_alarm_get_status();
            if (alarm) {
                merge_status(g_cache[CAT_ALARM], alarm);
                save_atomic("alarm", g_cache[CAT_ALARM]);
                cJSON_Delete(alarm);
            }
        }
        break;

    /* ---- 系统状态采集 cmd=601：更新全部设备 config ---- */
    case CMD_SYS_STATUS: /* 601 */
        config_manager_sync_device_status();
        break;

    /* ---- 服务器配置 cmd=605：serverUrl 可能变更 ---- */
    case CMD_SYS_CONFIG: /* 605 */
        printf("[CFG] cmd=605 服务器配置更新，保持当前 server.json\n");
        save_atomic("server", g_cache[CAT_SERVER]);
        break;

    default:
        /* 其他命令（如认证 cmd=11、校时 cmd=602 等）不触发 config 更新 */
        break;
    }

    pthread_mutex_unlock(&g_cfg_mutex);
}

void config_manager_update_mood(void)
{
    if (!g_inited) return;

    pthread_mutex_lock(&g_cfg_mutex);
    cJSON *mood = device_mood_get_status();
    if (mood) {
        merge_status(g_cache[CAT_MOOD], mood);
        save_atomic("mood", g_cache[CAT_MOOD]);
        cJSON_Delete(mood);
    }
    pthread_mutex_unlock(&g_cfg_mutex);
}

void config_manager_save_presets(void)
{
    if (!g_inited) return;
    preset_config_save(g_config_dir);
}

void config_manager_load_presets(void)
{
    if (!g_inited) return;
    preset_config_load(g_config_dir);
}

void config_manager_clear_presets(void)
{
    if (!g_inited) return;
    preset_config_clear(g_config_dir);
}

void config_manager_set_auth_token(const char *token)
{
    if (!g_inited || !token) return;

    pthread_mutex_lock(&g_cfg_mutex);
    cJSON *old = cJSON_DetachItemFromObject(g_cache[CAT_SERVER], "authToken");
    if (old) cJSON_Delete(old);
    cJSON_AddStringToObject(g_cache[CAT_SERVER], "authToken", token);
    save_atomic("server", g_cache[CAT_SERVER]);
    pthread_mutex_unlock(&g_cfg_mutex);
    printf("[CFG] authToken 已更新\n");
}

void config_manager_set_connected(bool connected)
{
    if (!g_inited) return;

    pthread_mutex_lock(&g_cfg_mutex);
    cJSON *old = cJSON_DetachItemFromObject(g_cache[CAT_SERVER], "connected");
    if (old) cJSON_Delete(old);
    cJSON_AddBoolToObject(g_cache[CAT_SERVER], "connected", connected);
    save_atomic("server", g_cache[CAT_SERVER]);
    pthread_mutex_unlock(&g_cfg_mutex);
}

bool config_manager_get_auth_token(char *dest, size_t size)
{
    if (!g_inited || !dest || size == 0) return false;

    pthread_mutex_lock(&g_cfg_mutex);
    bool ok = false;
    cJSON *token_item = cJSON_GetObjectItem(g_cache[CAT_SERVER], "authToken");
    if (cJSON_IsString(token_item) && token_item->valuestring[0] != '\0') {
        strncpy(dest, token_item->valuestring, size - 1);
        dest[size - 1] = '\0';
        ok = true;
    }
    pthread_mutex_unlock(&g_cfg_mutex);
    return ok;
}

void config_manager_deinit(void)
{
    if (!g_inited) return;

    pthread_mutex_lock(&g_cfg_mutex);
    for (int i = 0; i < CAT_COUNT; i++) {
        if (g_cache[i]) {
            cJSON_Delete(g_cache[i]);
            g_cache[i] = NULL;
        }
    }
    g_inited = false;
    pthread_mutex_unlock(&g_cfg_mutex);
    printf("[CFG] 配置管理器已释放\n");
}
