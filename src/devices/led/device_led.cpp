/**
 * @file    device_led.cpp
 * @brief   LED 字幕屏控制实现（V2.7 — 对接灵信视觉 ledplayer7 SDK）
 *
 *          cmd=201: 字幕播放 (textData/lightVal/showType/switchTime/displayStyle)
 *          cmd=202: 开关控制 (switch)
 *
 *          参数来源：lv.conf，已硬编码为内部常量，无需外部配置文件。
 *          SDK: 灵信视觉 ledplayer7 (libledplayer7.so)
 */

/* 先包含 C++ SDK 头文件（使用 C++ 类型） */
#include "ledplayer7.h"

/* LV_SetBrightness 在 .so 中存在但 ledplayer7.h 未声明，手动补 */
extern "C" int LV_SetBrightness(LPCOMMUNICATIONINFO pCommunicationInfo,
                                int BrightnessValue);

/* C 兼容的头文件 */
extern "C" {
#include "device_led.h"
#include "msg_builder.h"
#include "config.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <algorithm>
#include <string>
#include <vector>

/* ======================================================================== */
/*  lv.conf 参数硬编码                                                         */
/* ======================================================================== */

#define LED_IP              "192.168.88.199"
#define LED_TYPE_VAL        0       /* 0=6代T系A系XC系                         */
#define LED_WIDTH_VAL       96
#define LED_HEIGHT_VAL      16
#define LED_COLOR_TYPE_VAL  1       /* 3=三基色（全彩）                        */
#define LED_GRAY_LEVEL_VAL  0       /* 非C卡固定0                              */
#define LED_FONT_PATH       "./font/simsun.ttc"

#define LED_DEFAULT_FONT_SIZE  14
#define LED_DEFAULT_SPEED      20
#define LED_DEFAULT_DELAY      3

/* lightVal 上限 */
#define LIGHT_VAL_MAX       100

#ifndef ERR_PARAM_INVALID
#define ERR_PARAM_INVALID   1001
#endif

/* ======================================================================== */
/*  模块内部状态                                                               */
/* ======================================================================== */

static COMMUNICATIONINFO g_comm_info;
static bool g_inited = false;

/* 缓存当前 LED 状态，供 get_status 上报 */
static std::string  g_cur_text;
static int          g_cur_light_val     = 5;
static int          g_cur_show_type     = 0;
static int          g_cur_display_style = 0;
static bool         g_online            = false;
static int          g_fail_status       = 0;    /* 0=正常, 1=不在线, 2=响应异常 */

/* 互斥锁，保护状态变量的并发读写（worker 线程 + 心跳线程） */
static pthread_mutex_t g_led_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ======================================================================== */
/*  内部工具                                                                   */
/* ======================================================================== */

/**
 * @brief   根据 lightVal 缩放颜色值
 *          lightVal=100 → 全亮度，lightVal=0 → 最暗（但不为0，保证可见）
 */
static COLORREF scale_color(COLORREF color, int light_val)
{
    if (light_val >= LIGHT_VAL_MAX) return color;
    if (light_val <= 0) light_val = 1;

    double scale = (double)light_val / (double)LIGHT_VAL_MAX;

    int r = (int)(((color >> 0)  & 0xFF) * scale);
    int g = (int)(((color >> 8)  & 0xFF) * scale);
    int b = (int)(((color >> 16) & 0xFF) * scale);

    return (COLORREF)((b << 16) | (g << 8) | r);
}

/**
 * @brief   将 textData JSON 数组解析为文本向量（按 textindex 排序）
 */
static std::vector<std::string> parse_text_data(cJSON *text_data_arr)
{
    std::vector<std::pair<int, std::string>> items;

    if (cJSON_IsArray(text_data_arr)) {
        int n = cJSON_GetArraySize(text_data_arr);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(text_data_arr, i);
            if (!item) continue;

            cJSON *j_idx  = cJSON_GetObjectItem(item, "textindex");
            cJSON *j_text = cJSON_GetObjectItem(item, "text");

            int idx = cJSON_IsNumber(j_idx) ? j_idx->valueint : i;
            const char *txt = cJSON_IsString(j_text) ? j_text->valuestring : "";

            items.push_back({idx, std::string(txt)});
        }
    }

    /* 按 textindex 升序 */
    std::sort(items.begin(), items.end(),
              [](const std::pair<int,std::string> &a,
                 const std::pair<int,std::string> &b) {
                  return a.first < b.first;
              });

    std::vector<std::string> result;
    for (auto &p : items) {
        result.push_back(p.second);
    }
    return result;
}

/**
 * @brief   打印 SDK 错误信息
 */
static void print_led_error(int err_code, const char *ctx)
{
    if (err_code != 0) {
        char err_str[256];
        LV_GetError(err_code, 256, err_str);
        fprintf(stderr, "[LED] %s 失败 (code=%d): %s\n", ctx, err_code, err_str);
    }
}

/* ======================================================================== */
/*  公开接口实现（extern "C" 保证 C 调用者兼容）                                */
/* ======================================================================== */

extern "C" {

int device_led_init(const char *uart_dev, int baudrate,
                    const char *net_ip, int net_port)
{
    (void)uart_dev;
    (void)baudrate;
    (void)net_port;

    /* 使用 config.h 传入的 IP，空则回退硬编码默认值 */
    const char *ip = (net_ip && net_ip[0] != '\0') ? net_ip : LED_IP;

    printf("[LED] 初始化 ledplayer7 SDK: IP=%s, %dx%d, color=%d, gray=%d\n",
           ip, LED_WIDTH_VAL, LED_HEIGHT_VAL,
           LED_COLOR_TYPE_VAL, LED_GRAY_LEVEL_VAL);

    /* 初始化屏类型（RGB顺序=0，默认） */
    LV_InitLed(LED_TYPE_VAL, 0);

    /* 填充通讯参数（TCP 固定 IP） */
    memset(&g_comm_info, 0, sizeof(g_comm_info));
    g_comm_info.LEDType   = LED_TYPE_VAL;
    g_comm_info.SendType  = 0;          /* TCP 固定IP通讯 */
    strcpy(g_comm_info.IpStr, ip);
    g_comm_info.LedNumber = 1;
    g_comm_info.Commport  = 0;
    g_comm_info.Baud      = 0;

    g_inited = true;
    g_online = true;

    printf("[LED] 初始化完成 (TCP → %s)\n", ip);
    return 0;
}

int device_led_execute(const cmd_t *cmd, cJSON **resp)
{
    if (!g_inited) {
        if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, cmd->cmd_id, "LED 未初始化");
        return -1;
    }

    printf("[LED] cmd=%d\n", cmd->cmd_id);

    switch (cmd->cmd_id) {

    /* ==================================================================== */
    /*  cmd=201  字幕播放                                                      */
    /* ==================================================================== */
    case 201: {
        cJSON *light_item = cJSON_GetObjectItem(cmd->root, "lightVal");
        int light_val     = cJSON_IsNumber(light_item) ? light_item->valueint : g_cur_light_val;
        int show_type     = cmd_get_int(cmd, "showType", 0);
        int switch_time   = cmd_get_int(cmd, "switchTime", 5);
        int display_style = cmd_get_int(cmd, "displayStyle", 0);
        cJSON *text_data  = cJSON_GetObjectItem(cmd->root, "textData");

        if (light_val > LIGHT_VAL_MAX) light_val = LIGHT_VAL_MAX;
        if (light_val < 30)            light_val = 30;

        /* 定时轮换仅适用于静止模式，强制覆盖 */
        if (show_type == 2) display_style = 0;

        std::vector<std::string> texts = parse_text_data(text_data);

        printf("[LED] 字幕播放: %zu 条文本, light=%d showType=%d"
               " switchTime=%d displayStyle=%d\n",
               texts.size(), light_val, show_type, switch_time, display_style);

        if (texts.empty()) {
            if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 201,
                                                 "textData 为空");
            return -1;
        }

        /* ---- 拼接文字 ---- */
        std::string combined;
        for (size_t i = 0; i < texts.size(); i++) {
            if (i > 0) {
                combined += (display_style == 1) ? "  " : "\n";
            }
            combined += texts[i];
        }
        printf("[LED] 显示文字: \"%s\"\n", combined.c_str());

        /* ---- 创建节目 ---- */
        HPROGRAM hProgram = LV_CreateProgramEx(LED_WIDTH_VAL, LED_HEIGHT_VAL,
                                                LED_COLOR_TYPE_VAL,
                                                LED_GRAY_LEVEL_VAL, 0);
        if (!hProgram) {
            if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201,
                                                 "创建节目句柄失败");
            return -1;
        }

        int nResult;
        int nProgramNo = 0;

        nResult = LV_AddProgram(hProgram, nProgramNo, 0, 255); /* LoopCount 最大 255 */
        if (nResult) {
            print_led_error(nResult, "LV_AddProgram");
            LV_DeleteProgram(hProgram);
            char err[128];
            snprintf(err, sizeof(err), "添加节目失败 SDK=%d", nResult);
            if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201, err);
            return -1;
        }

        /* ---- 设置节目定时（必须调用，否则 switchTime 等无效） ---- */
        {
            PROGRAMTIME prog_time;
            memset(&prog_time, 0, sizeof(prog_time));
            prog_time.EnableFlag = 0;  /* 不启用日期/时间/星期限制，立即播放 */
            nResult = LV_SetProgramTime(hProgram, nProgramNo, &prog_time);
            if (nResult) {
                print_led_error(nResult, "LV_SetProgramTime");
                /* 非致命，继续 */
            }
        }

        /* ---- 添加文字区域 ---- */
        COLORREF font_color = scale_color(COLOR_RED, light_val);

        if (display_style == 1) {
            /* 左右滚动：快速单行文本（连续左移） */
            AREARECT area;
            area.left   = 16;
            area.top    = 0;
            area.width  = 80;  //96
            area.height = 16;  //16

            FONTPROP font;
            memset(&font, 0, sizeof(font));
            strcpy(font.FontPath, LED_FONT_PATH);
            font.FontSize  = LED_DEFAULT_FONT_SIZE;
            font.FontColor = font_color;

            nResult = LV_QuickAddSingleLineTextArea(
                hProgram, nProgramNo, 1, &area,
                ADDTYPE_STRING, combined.c_str(), &font, LED_DEFAULT_SPEED);
            if (nResult) {
                print_led_error(nResult, "LV_QuickAddSingleLineTextArea");
                LV_DeleteProgram(hProgram);
                char err[128];
                snprintf(err, sizeof(err), "添加滚动文本失败 SDK=%d", nResult);
                if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201, err);
                return -1;
            }
        } else if (display_style == 2) {
            /* ---- 上下滚动：单行文本区域 + 连续上移 (InStyle=8) ---- */
            /*
             * 参照左右滚动的 LV_QuickAddSingleLineTextArea 模式，
             * 使用 LV_AddSingleLineTextToImageTextArea + InStyle=8。
             * 多条文本用空格拼接为连续字符串，让 SDK 在区域内换行后滚动。
             */
            AREARECT area;
            area.left   = 16;
            area.top    = 0;
            area.width  = 80;
            area.height = 16;

            nResult = LV_AddImageTextArea(hProgram, nProgramNo, 1, &area, 1);
            if (nResult) {
                print_led_error(nResult, "LV_AddImageTextArea");
                LV_DeleteProgram(hProgram);
                char err[128];
                snprintf(err, sizeof(err), "添加图文区域失败(上滚) SDK=%d", nResult);
                if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201, err);
                return -1;
            }

            FONTPROP font;
            memset(&font, 0, sizeof(font));
            strcpy(font.FontPath, LED_FONT_PATH);
            font.FontSize  = LED_DEFAULT_FONT_SIZE;
            font.FontColor = font_color;

            PLAYPROP play;
            play.InStyle   = 8;  /* 8=连续上移 */
            play.OutStyle  = 0;
            play.Speed     = LED_DEFAULT_SPEED;
            play.DelayTime = 0;  /* 连续特技下无效，显式置 0 */

            nResult = LV_AddSingleLineTextToImageTextArea(
                hProgram, nProgramNo, 1,
                ADDTYPE_STRING, combined.c_str(),
                &font, &play);
            if (nResult) {
                print_led_error(nResult,
                                "LV_AddSingleLineTextToImageTextArea(上移)");
                LV_DeleteProgram(hProgram);
                char err[128];
                snprintf(err, sizeof(err), "添加上下滚动文本失败 SDK=%d", nResult);
                if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201, err);
                return -1;
            }
        } else {
            /* ---- 静止显示 / 定时轮换：多行文本区域 ---- */
            AREARECT area;
            area.left   = 16;
            area.top    = 1;
            area.width  = 78;
            area.height = 14;

            nResult = LV_AddImageTextArea(hProgram, nProgramNo, 1, &area, 1);
            if (nResult) {
                print_led_error(nResult, "LV_AddImageTextArea");
                LV_DeleteProgram(hProgram);
                char err[128];
                snprintf(err, sizeof(err), "添加图文区域失败(静态) SDK=%d", nResult);
                if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201, err);
                return -1;
            }

            FONTPROP font;
            memset(&font, 0, sizeof(font));
            strcpy(font.FontPath, LED_FONT_PATH);
            font.FontSize  = LED_DEFAULT_FONT_SIZE;
            font.FontColor = font_color;

            PLAYPROP play;
            play.DelayTime = (show_type == 2) ? switch_time : LED_DEFAULT_DELAY;
            play.InStyle   = 0;  /* 0=立即显示 */
            play.OutStyle  = 0;
            play.Speed     = LED_DEFAULT_SPEED;

            if (show_type == 2 && texts.size() > 1) {
                /* 定时轮换：每条文本作为独立静态页 */
                for (size_t i = 0; i < texts.size(); i++) {
                    nResult = LV_AddStaticTextToImageTextArea(
                        hProgram, nProgramNo, 1,
                        ADDTYPE_STRING, texts[i].c_str(),
                        &font, switch_time, 2/*水平居中*/, TRUE/*垂直居中*/);
                    if (nResult) {
                        print_led_error(nResult,
                                        "LV_AddStaticTextToImageTextArea");
                    }
                }
            } else {
                /* 多行文本静止显示 */
                nResult = LV_AddMultiLineTextToImageTextArea(
                    hProgram, nProgramNo, 1,
                    ADDTYPE_STRING, combined.c_str(),
                    &font, &play, 2/*水平居中*/, FALSE/*置顶*/);
                if (nResult) {
                    print_led_error(nResult,
                                    "LV_AddMultiLineTextToImageTextArea");
                    LV_DeleteProgram(hProgram);
                    char err[128];
                    snprintf(err, sizeof(err), "添加多行文本失败 SDK=%d", nResult);
                    if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201, err);
                    return -1;
                }
            }
        }

        /* ---- 设置亮度（SDK 硬件亮度 0~15） ---- */
        {
            int hw_brightness = (light_val * 15) / LIGHT_VAL_MAX;
            if (hw_brightness < 0)  hw_brightness = 0;
            if (hw_brightness > 15) hw_brightness = 15;
            nResult = LV_SetBrightness(&g_comm_info, hw_brightness);
            if (nResult) {
                print_led_error(nResult, "LV_SetBrightness");
                /* 非致命错误，继续发送节目 */
            }
        }

        /* ---- 发送 ---- */
        nResult = LV_Send(&g_comm_info, hProgram);
        LV_DeleteProgram(hProgram);

        if (nResult) {
            print_led_error(nResult, "LV_Send");
            pthread_mutex_lock(&g_led_mutex);
            g_online = false;
            g_fail_status = 2;  /* 响应异常 */
            pthread_mutex_unlock(&g_led_mutex);
            char err[128];
            snprintf(err, sizeof(err), "发送节目失败 SDK=%d", nResult);
            if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 201, err);
            return -1;
        }

        printf("[LED] 节目发送成功\n");

        /* ---- 更新内部状态缓存 ---- */
        pthread_mutex_lock(&g_led_mutex);
        g_online            = true;
        g_fail_status       = 0;  /* 恢复正常 */
        g_cur_text          = combined;
        g_cur_light_val     = light_val;
        g_cur_show_type     = show_type;
        g_cur_display_style = display_style;
        pthread_mutex_unlock(&g_led_mutex);

        if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 201, "success");
        return 0;
    }

    /* ==================================================================== */
    /*  cmd=202  开关控制                                                      */
    /* ==================================================================== */
    case 202: {
        int sw = cmd_get_int(cmd, "switch", 0);
        printf("[LED] 开关: %d\n", sw);

        /* 参数校验：仅允许 0 或 1 */
        if (sw != 0 && sw != 1) {
            if (resp) *resp = msg_build_cmd_response(ERR_PARAM_INVALID, 202,
                                                     "无效的开关值，仅支持0或1");
            return -1;
        }

        /*
         * LV_PowerOnOff: 0=开屏, 1=关屏, 2=重启
         * API spec: switch=1→开, switch=0→关
         */
        int onoff = (sw == 1) ? 0 : 1;

        int nResult = LV_PowerOnOff(&g_comm_info, onoff);
        if (nResult) {
            print_led_error(nResult, "LV_PowerOnOff");
            pthread_mutex_lock(&g_led_mutex);
            g_online = false;
            g_fail_status = 2;  /* 响应异常 */
            pthread_mutex_unlock(&g_led_mutex);
            if (resp) *resp = msg_build_cmd_response(ERR_DEVICE_COMM, 202,
                                                 "LED 开关控制失败");
            return -1;
        }

        printf("[LED] 开关控制成功: %s\n", sw ? "开屏" : "关屏");
        pthread_mutex_lock(&g_led_mutex);
        g_fail_status = 0;  /* 恢复正常 */
        pthread_mutex_unlock(&g_led_mutex);
        if (resp) *resp = msg_build_cmd_response(ERR_SUCCESS, 202, "success");
        return 0;
    }

    default:
        if (resp) *resp = msg_build_cmd_response(ERR_NOT_SUPPORTED, cmd->cmd_id,
                                             "LED 不支持该命令");
        return -1;
    }
}

cJSON *device_led_get_status(void)
{
    /*
     * V3.1: 使用 trylock 防阻塞。
     * 心跳和 cmd=601 在主线程调用，LED worker 可能正在执行 LV_Send
     * 并短暂持有 g_led_mutex 更新状态。trylock 失败时无锁读取。
     */
    std::string txt;
    int lv, st, ds;
    bool onl;

    if (pthread_mutex_trylock(&g_led_mutex) == 0) {
        txt = g_cur_text;
        lv  = g_cur_light_val;
        st  = g_cur_show_type;
        ds  = g_cur_display_style;
        onl = g_online;
        pthread_mutex_unlock(&g_led_mutex);
    } else {
        txt = g_cur_text;
        lv  = g_cur_light_val;
        st  = g_cur_show_type;
        ds  = g_cur_display_style;
        onl = g_online;
    }

    cJSON *status = cJSON_CreateObject();
    cJSON_AddBoolToObject  (status, "online",  onl);

    /* textData 数组 */
    cJSON *arr = cJSON_CreateArray();
    if (!txt.empty()) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "textindex", 0);
        cJSON_AddStringToObject(item, "text", txt.c_str());
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(status, "textData", arr);

    cJSON_AddNumberToObject(status, "lightVal",     lv);
    cJSON_AddNumberToObject(status, "showType",     st);
    cJSON_AddNumberToObject(status, "displayStyle", ds);

    return status;
}

int device_led_get_fail_status(void)
{
    return g_fail_status;
}

void device_led_apply_config(const char *text, int light_val,
                              int show_type, int display_style)
{
    pthread_mutex_lock(&g_led_mutex);

    if (text && text[0] != '\0') {
        g_cur_text = text;
    }
    if (light_val >= 0 && light_val <= LIGHT_VAL_MAX) {
        g_cur_light_val = light_val;
    }
    g_cur_show_type     = show_type;
    g_cur_display_style = display_style;

    pthread_mutex_unlock(&g_led_mutex);

    printf("[LED] 配置已恢复: text=\"%s\" light=%d showType=%d displayStyle=%d\n",
           g_cur_text.c_str(), g_cur_light_val, g_cur_show_type, g_cur_display_style);
}

void device_led_deinit(void)
{
    printf("[LED] LED 模块释放\n");
    g_inited = false;
    g_online = false;
}

} /* extern "C" */
