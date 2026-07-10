/**
 * @file    cmd_parser.c
 * @brief   JSON 命令解析实现（V2.6）
 *
 *          兼容两种入站格式：
 *          1. cmd 整数格式：{"cmd": 101, "token": "...", "field1": ..., ...}
 *          2. 直接字段格式：{"state": "on", "type": "breath"}（氛围灯等）
 */

#include "cmd_parser.h"

#include <string.h>
#include <stdlib.h>

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

bool cmd_parser_parse(const char *json_str, cmd_t *cmd)
{
    if (!json_str || !cmd) {
        return false;
    }

    memset(cmd, 0, sizeof(cmd_t));
    cmd->cmd_id = -1;

    /* 解析 JSON */
    cmd->root = cJSON_Parse(json_str);
    if (!cmd->root) {
        return false;
    }

    /* 提取 cmd 字段（选填，部分命令如氛围灯不传 cmd） */
    cJSON *cmd_item = cJSON_GetObjectItem(cmd->root, "cmd");
    if (cJSON_IsNumber(cmd_item)) {
        cmd->cmd_id = cmd_item->valueint;
    }

    /* 提取 token 字段（选填） */
    cJSON *token_item = cJSON_GetObjectItem(cmd->root, "token");
    if (cJSON_IsString(token_item)) {
        cmd->token = cJSON_GetStringValue(token_item);
    }

    return true;
}

/* ======================================================================== */
/*  字段提取（直接从 root 取值）                                               */
/* ======================================================================== */

const char *cmd_get_string(const cmd_t *cmd, const char *key)
{
    if (!cmd || !cmd->root || !key) return NULL;
    cJSON *item = cJSON_GetObjectItem(cmd->root, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

int cmd_get_int(const cmd_t *cmd, const char *key, int def)
{
    if (!cmd || !cmd->root || !key) return def;
    cJSON *item = cJSON_GetObjectItem(cmd->root, key);
    return cJSON_IsNumber(item) ? item->valueint : def;
}

double cmd_get_double(const cmd_t *cmd, const char *key, double def)
{
    if (!cmd || !cmd->root || !key) return def;
    cJSON *item = cJSON_GetObjectItem(cmd->root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : def;
}

bool cmd_get_bool(const cmd_t *cmd, const char *key, bool def)
{
    if (!cmd || !cmd->root || !key) return def;
    cJSON *item = cJSON_GetObjectItem(cmd->root, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsString(item)) {
        const char *val = cJSON_GetStringValue(item);
        if (strcasecmp(val, "on")   == 0) return true;
        if (strcasecmp(val, "true") == 0) return true;
        if (strcasecmp(val, "off")  == 0) return false;
        if (strcasecmp(val, "false")== 0) return false;
    }
    return def;
}

/* ------ data 子对象取值（兼容旧格式） ------ */

const char *cmd_get_data_string(const cmd_t *cmd, const char *key)
{
    if (!cmd || !cmd->root || !key) return NULL;
    cJSON *data = cJSON_GetObjectItem(cmd->root, "data");
    if (!cJSON_IsObject(data)) return NULL;
    cJSON *item = cJSON_GetObjectItem(data, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

int cmd_get_data_int(const cmd_t *cmd, const char *key, int def)
{
    if (!cmd || !cmd->root || !key) return def;
    cJSON *data = cJSON_GetObjectItem(cmd->root, "data");
    if (!cJSON_IsObject(data)) return def;
    cJSON *item = cJSON_GetObjectItem(data, key);
    return cJSON_IsNumber(item) ? item->valueint : def;
}

/* ======================================================================== */
/*  释放                                                                      */
/* ======================================================================== */

void cmd_parser_free(cmd_t *cmd)
{
    if (cmd && cmd->root) {
        cJSON_Delete(cmd->root);
        cmd->root  = NULL;
        cmd->token = NULL;
    }
}
