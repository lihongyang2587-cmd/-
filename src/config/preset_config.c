/**
 * @file    preset_config.c
 * @brief   预置位 YAML 配置文件读写实现
 *
 *          手动格式化/解析 YAML，不引入第三方库。
 *          结构固定：groups → groupNum + points[] → index/horAngle/verAngle
 */

#include "preset_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>

#include "cJSON.h"
#include "device_ptz.h"

/* ======================================================================== */
/*  内部常量                                                                   */
/* ======================================================================== */

#define MAX_LINE_LEN    256
#define MAX_POINTS      16

/* ======================================================================== */
/*  内部工具                                                                   */
/* ======================================================================== */

/** 去除行首空白，返回首个非空字符指针 */
static const char *skip_leading_spaces(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/** 去除行尾换行符 */
static void strip_newline(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/** 判断行是否为注释或空行 */
static int is_skip_line(const char *s)
{
    s = skip_leading_spaces(s);
    return (*s == '\0' || *s == '#');
}

/** 从 "  key: value" 行提取整数值 */
static int parse_int_value(const char *line, const char *key, int *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s:", key);

    const char *pos = strstr(line, pattern);
    if (!pos) return -1;

    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) pos++;

    char *end = NULL;
    long val = strtol(pos, &end, 10);
    if (end == pos) return -1;

    *out = (int)val;
    return 0;
}

/** 构建文件路径 */
static void make_path(const char *config_dir, char *buf, size_t size)
{
    snprintf(buf, size, "%s/preset_positions.yaml", config_dir);
}

/* ======================================================================== */
/*  写入 YAML                                                                 */
/* ======================================================================== */

int preset_config_save(const char *config_dir)
{
    char path[512], tmp[520];
    make_path(config_dir, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        fprintf(stderr, "[PRESET] 无法写入 %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    fprintf(fp, "# 云台预置位配置文件\n");
    fprintf(fp, "# 组1 -> Pelco-D 预置位 1~16\n");
    fprintf(fp, "# 组2 -> Pelco-D 预置位 33~48\n");
    fprintf(fp, "groups:\n");

    for (int gn = 1; gn <= 2; gn++) {
        fprintf(fp, "  - groupNum: %d\n", gn);
        fprintf(fp, "    points:\n");

        ptz_preset_group_t group;
        if (device_ptz_get_preset_group(gn, &group) == 0 && group.count > 0) {
            for (int i = 0; i < group.count; i++) {
                fprintf(fp, "      - index: %d\n",    group.points[i].index);
                fprintf(fp, "        horAngle: %d\n", group.points[i].hor_angle);
                fprintf(fp, "        verAngle: %d\n", group.points[i].ver_angle);
                fprintf(fp, "        success: %s\n",  group.points[i].success ? "true" : "false");
            }
        } else {
            /* 空组：写空列表标记 */
            fprintf(fp, "      []\n");
        }
    }

    fclose(fp);

    if (rename(tmp, path) != 0) {
        fprintf(stderr, "[PRESET] rename 失败 %s -> %s: %s\n",
                tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    printf("[PRESET] 预置位已保存到 %s\n", path);
    return 0;
}

/* ======================================================================== */
/*  读取 YAML（简单状态机）                                                     */
/* ======================================================================== */

int preset_config_load(const char *config_dir)
{
    char path[512];
    make_path(config_dir, path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[PRESET] %s 不存在，跳过预置位恢复\n", path);
        return -1;
    }

    printf("[PRESET] 加载 %s\n", path);

    char line[MAX_LINE_LEN];

    /* 状态机阶段 */
    enum { ST_FIND_GROUPS, ST_IN_GROUP, ST_IN_POINTS } state = ST_FIND_GROUPS;

    int  cur_group_num   = 0;
    int  cur_point_count = 0;
    int  cur_index       = 0;
    int  cur_hor_angle   = 0;
    int  cur_ver_angle   = 0;
    int  cur_success     = 1;   /* 默认 true，兼容旧格式无此字段 */
    int  has_index       = 0;
    int  has_hor         = 0;
    int  has_ver         = 0;
    int  has_success     = 0;

    int groups_loaded    = 0;

    /*
     * 收集一个预置位组的临时缓冲区，读取完一组后
     * 调用 device_ptz_load_preset_group()。
     */
    struct {
        int index;
        int hor_angle;
        int ver_angle;
        int success;
    } temp_points[MAX_POINTS];

    while (fgets(line, sizeof(line), fp)) {
        strip_newline(line);

        if (is_skip_line(line)) continue;

        const char *s = skip_leading_spaces(line);
        int indent = (int)(s - line);  /* 缩进空格数 */

        switch (state) {

        case ST_FIND_GROUPS:
            if (strncmp(s, "groups:", 7) == 0) {
                state = ST_IN_GROUP;
                cur_group_num   = 0;
                cur_point_count = 0;
                has_index = has_hor = has_ver = 0;
            }
            break;

        case ST_IN_GROUP:
            /* 检测到 "  - groupNum: N" → 新组开始 */
            if (indent == 2 && strncmp(s, "- groupNum:", 11) == 0) {
                /* 上一组有数据则提交 */
                if (cur_group_num >= 1 && cur_group_num <= 2 &&
                    cur_point_count > 0) {
                    /* 构建 JSON 传给 device_ptz_load_preset_group */
                    cJSON *json = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json, "groupNum", cur_group_num);
                    cJSON *arr = cJSON_AddArrayToObject(json, "groupData");
                    for (int i = 0; i < cur_point_count; i++) {
                        cJSON *pt = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pt, "index",    temp_points[i].index);
                        cJSON_AddNumberToObject(pt, "horAngle", temp_points[i].hor_angle);
                        cJSON_AddNumberToObject(pt, "verAngle", temp_points[i].ver_angle);
                        cJSON_AddBoolToObject  (pt, "success",  temp_points[i].success != 0);
                        cJSON_AddItemToArray(arr, pt);
                    }
                    if (device_ptz_load_preset_group(cur_group_num, json) == 0) {
                        groups_loaded++;
                    }
                    cJSON_Delete(json);
                }

                /* 开始新组 */
                parse_int_value(s, "groupNum", &cur_group_num);
                cur_point_count = 0;
                has_index = has_hor = has_ver = has_success = 0;
                cur_success = 1;
                state = ST_IN_GROUP;
            }
            /* 检测到 "    points:" → 进入点位解析 */
            else if (indent == 4 && strncmp(s, "points:", 7) == 0) {
                state = ST_IN_POINTS;
            }
            /* 空列表 "      []" → 记录空组但无 points，保持在 ST_IN_GROUP */
            break;

        case ST_IN_POINTS:
            /* 检测到 "      - index: N" → 新点位 */
            if (indent == 6 && strncmp(s, "- index:", 8) == 0) {
                /* 上一组的上一个点位如果完整则保存 */
                if (has_index && has_hor && has_ver &&
                    cur_point_count < MAX_POINTS) {
                    temp_points[cur_point_count].index     = cur_index;
                    temp_points[cur_point_count].hor_angle = cur_hor_angle;
                    temp_points[cur_point_count].ver_angle = cur_ver_angle;
                    temp_points[cur_point_count].success   = cur_success;
                    cur_point_count++;
                }
                has_index = has_hor = has_ver = has_success = 0;
                cur_success = 1;   /* 重置默认值 */
                parse_int_value(s, "index", &cur_index);
                has_index = 1;
            }
            else if (indent == 8 && has_index) {
                if (strstr(s, "horAngle:")) {
                    parse_int_value(s, "horAngle", &cur_hor_angle);
                    has_hor = 1;
                }
                else if (strstr(s, "verAngle:")) {
                    parse_int_value(s, "verAngle", &cur_ver_angle);
                    has_ver = 1;
                }
                else if (strstr(s, "success:")) {
                    /* 兼容 true/false 和 1/0 两种写法 */
                    if (strstr(s, "true") || strstr(s, "1")) {
                        cur_success = 1;
                    } else {
                        cur_success = 0;
                    }
                    has_success = 1;
                }
                /*
                 * 不在这里立即保存——success 字段在 verAngle 之后，
                 * 等下一个 "- index:" 或 section 结束时再保存。
                 */
            }
            /* 检测到 "  - groupNum: N" (新组开始) → 回退到 ST_IN_GROUP */
            else if (indent == 2 && strncmp(s, "- groupNum:", 11) == 0) {
                /* 先提交当前组的最后一个点位 */
                if (has_index && has_hor && has_ver &&
                    cur_point_count < MAX_POINTS) {
                    temp_points[cur_point_count].index     = cur_index;
                    temp_points[cur_point_count].hor_angle = cur_hor_angle;
                    temp_points[cur_point_count].ver_angle = cur_ver_angle;
                    temp_points[cur_point_count].success   = cur_success;
                    cur_point_count++;
                }
                /* 提交当前组 */
                if (cur_group_num >= 1 && cur_group_num <= 2 &&
                    cur_point_count > 0) {
                    cJSON *json = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json, "groupNum", cur_group_num);
                    cJSON *arr = cJSON_AddArrayToObject(json, "groupData");
                    for (int i = 0; i < cur_point_count; i++) {
                        cJSON *pt = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pt, "index",    temp_points[i].index);
                        cJSON_AddNumberToObject(pt, "horAngle", temp_points[i].hor_angle);
                        cJSON_AddNumberToObject(pt, "verAngle", temp_points[i].ver_angle);
                        cJSON_AddBoolToObject  (pt, "success",  temp_points[i].success != 0);
                        cJSON_AddItemToArray(arr, pt);
                    }
                    if (device_ptz_load_preset_group(cur_group_num, json) == 0) {
                        groups_loaded++;
                    }
                    cJSON_Delete(json);
                }
                /* 开始新组 */
                parse_int_value(s, "groupNum", &cur_group_num);
                cur_point_count = 0;
                has_index = has_hor = has_ver = has_success = 0;
                cur_success = 1;
                state = ST_IN_GROUP;
            }
            /* 不是点位数据 → 退回 ST_IN_GROUP */
            else {
                state = ST_IN_GROUP;
            }
            break;
        }
    }

    /* 文件结束，提交最后收集的组 */
    if (has_index && has_hor && has_ver &&
        cur_point_count < MAX_POINTS) {
        temp_points[cur_point_count].index     = cur_index;
        temp_points[cur_point_count].hor_angle = cur_hor_angle;
        temp_points[cur_point_count].ver_angle = cur_ver_angle;
        temp_points[cur_point_count].success   = cur_success;
        cur_point_count++;
    }

    if (cur_group_num >= 1 && cur_group_num <= 2 &&
        cur_point_count > 0) {
        cJSON *json = cJSON_CreateObject();
        cJSON_AddNumberToObject(json, "groupNum", cur_group_num);
        cJSON *arr = cJSON_AddArrayToObject(json, "groupData");
        for (int i = 0; i < cur_point_count; i++) {
            cJSON *pt = cJSON_CreateObject();
            cJSON_AddNumberToObject(pt, "index",    temp_points[i].index);
            cJSON_AddNumberToObject(pt, "horAngle", temp_points[i].hor_angle);
            cJSON_AddNumberToObject(pt, "verAngle", temp_points[i].ver_angle);
            cJSON_AddBoolToObject  (pt, "success",  temp_points[i].success != 0);
            cJSON_AddItemToArray(arr, pt);
        }
        if (device_ptz_load_preset_group(cur_group_num, json) == 0) {
            groups_loaded++;
        }
        cJSON_Delete(json);
    }

    fclose(fp);

    if (groups_loaded > 0) {
        printf("[PRESET] 从 %s 恢复了 %d 组预置位\n", path, groups_loaded);
        return 0;
    }

    printf("[PRESET] %s 中无有效预置位数据\n", path);
    return -1;
}

/* ======================================================================== */
/*  清除 YAML 文件                                                             */
/* ======================================================================== */

void preset_config_clear(const char *config_dir)
{
    char path[512];
    make_path(config_dir, path, sizeof(path));

    if (unlink(path) == 0) {
        printf("[PRESET] 已清除 %s\n", path);
    } else if (errno != ENOENT) {
        fprintf(stderr, "[PRESET] 清除 %s 失败: %s\n", path, strerror(errno));
    }
}
