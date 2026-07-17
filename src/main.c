/**
 * @file    main.c
 * @brief   机器人控制板主程序（V2.6 — 修复事件处理与即时状态上报）
 *
 *          启动流程：
 *          1. 注册信号处理
 *          2. 初始化各设备模块
 *          3. 创建环形缓冲区
 *          4. 启动 WebSocket 客户端
 *          5. 发送认证请求 (cmd=11) 并等待 token
 *          6. 启动心跳定时器
 *          7. 进入主循环：取消息 → 解析 → 分发 → 发送回复
 *             · cmd 指令消息 → 执行并回复
 *             · event ack 消息 → 仅记录日志（服务端确认）
 *             · 每轮轮询设备状态变更标志，有变更立即上报
 */

#include "main.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include "config.h"
#include "ring_buffer.h"
#include "ws_client.h"
#include "ws_send_queue.h"
#include "task_queue.h"
#include "cmd_parser.h"
#include "cmd_dispatch.h"
#include "msg_builder.h"
#include "heartbeat.h"
#include "config_manager.h"
#include "cJSON.h"

#include "device_ptz.h"
#include "device_speaker.h"
#include "device_alarm.h"
#include "device_led.h"
#include "device_mood.h"
#include "device_system.h"

/* ======================================================================== */
/*  全局变量                                                                  */
/* ======================================================================== */

volatile bool g_running = true;

static ring_buffer_t  *g_rx_buffer  = NULL;
static ws_client_t    *g_ws_client  = NULL;
static heartbeat_t    *g_heartbeat  = NULL;

/* ---- 多线程外设控制 ---- */
static task_queue_t    *g_ptz_queue   = NULL;
static task_queue_t    *g_led_queue   = NULL;
static task_queue_t    *g_spk_queue   = NULL;
static task_queue_t    *g_alarm_queue = NULL;
static pthread_t        g_ptz_thread;
static pthread_t        g_led_thread;
static pthread_t        g_spk_thread;
static pthread_t        g_alarm_thread;
static ws_send_queue_t *g_send_queue  = NULL;
static bool              g_workers_inited = false;

/* ---- 重连重认证状态（volatile：mongoose 线程写入，主线程读取） ---- */
static volatile bool g_need_reauth    = false;   /* 重连后需重新鉴权               */
static bool          g_ws_was_connected = false; /* 上一次连接状态（边沿检测用）   */

/* ======================================================================== */
/*  信号处理                                                                  */
/* ======================================================================== */

static void signal_handler(int sig)
{
    (void)sig;
    const char msg[] = "[MAIN] 收到退出信号\n";
    if (write(STDOUT_FILENO, msg, sizeof(msg) - 1)) {}
    g_running = false;
}

/* ======================================================================== */
/*  初始化所有设备                                                            */
/* ======================================================================== */

static int init_all_devices(void)
{
    int ret;

    ret = device_ptz_init(PTZ_UART_DEV, PTZ_UART_BAUDRATE);
    if (ret != 0) return ret;

    ret = device_speaker_init();
    if (ret != 0) return ret;

    ret = device_alarm_init(ALARM_GPIO_CHIP, ALARM_GPIO_PIN);
    if (ret != 0) return ret;

    ret = device_led_init(LED_UART_DEV, LED_UART_BAUDRATE,
                          LED_NET_IP, LED_NET_PORT);
    if (ret != 0) return ret;

    ret = device_mood_init();
    if (ret != 0) return ret;

    printf("[MAIN] 所有设备初始化完成\n");
    return 0;
}

/* ======================================================================== */
/*  释放所有资源                                                              */
/* ======================================================================== */

static void cleanup(void)
{
    /* 1. 停止心跳 */
    if (g_heartbeat) { heartbeat_stop(g_heartbeat); g_heartbeat = NULL; }

    /* 2. shutdown 所有 worker 队列（唤醒阻塞的 worker 线程） */
    if (g_ptz_queue)   task_queue_shutdown(g_ptz_queue);
    if (g_led_queue)   task_queue_shutdown(g_led_queue);
    if (g_spk_queue)   task_queue_shutdown(g_spk_queue);
    if (g_alarm_queue) task_queue_shutdown(g_alarm_queue);

    /* 3. join 所有 worker 线程（仅在成功启动后） */
    if (g_workers_inited) {
        pthread_join(g_ptz_thread,   NULL);
        pthread_join(g_led_thread,   NULL);
        pthread_join(g_spk_thread,   NULL);
        pthread_join(g_alarm_thread, NULL);
    }

    /* 4. 销毁 task queue */
    if (g_ptz_queue)   { task_queue_destroy(g_ptz_queue);   g_ptz_queue   = NULL; }
    if (g_led_queue)   { task_queue_destroy(g_led_queue);   g_led_queue   = NULL; }
    if (g_spk_queue)   { task_queue_destroy(g_spk_queue);   g_spk_queue   = NULL; }
    if (g_alarm_queue) { task_queue_destroy(g_alarm_queue); g_alarm_queue = NULL; }

    /* 5. 停止 send_queue（排空 + 停止发送线程） */
    if (g_send_queue)  { ws_send_queue_deinit(g_send_queue); g_send_queue = NULL; }

    /* 6. WebSocket 断开 */
    if (g_ws_client) { ws_client_deinit(g_ws_client); g_ws_client = NULL; }

    /* 7. 释放各设备 */
    device_ptz_deinit();
    device_speaker_deinit();
    device_alarm_deinit();
    device_led_deinit();
    device_mood_deinit();

    /* 8. 销毁 ring_buffer */
    if (g_rx_buffer) { ring_buffer_destroy(g_rx_buffer); g_rx_buffer = NULL; }
}

/* ======================================================================== */
/*  Worker 线程 —— 多线程外设控制                                               */
/* ======================================================================== */

/**
 * @brief   设备任务（深拷贝命令，worker 线程独立持有）
 */
typedef struct {
    cmd_t    cmd;
    int      cmd_id;
} device_task_t;

static device_task_t *device_task_create(const cmd_t *cmd)
{
    device_task_t *t = (device_task_t *)calloc(1, sizeof(device_task_t));
    if (!t) return NULL;
    t->cmd_id     = cmd->cmd_id;
    t->cmd.cmd_id = cmd->cmd_id;
    t->cmd.token  = NULL;   /* worker 不使用 token，仅保证非野指针 */
    t->cmd.root   = cJSON_Duplicate(cmd->root, 1);
    return t;
}

static void device_task_destroy(device_task_t *t)
{
    if (t) {
        if (t->cmd.root) cJSON_Delete(t->cmd.root);
        free(t);
    }
}

/* ---- PTZ Worker ---- */
static void *ptz_worker_thread(void *arg)
{
    (void)arg;
    printf("[WORKER-PTZ] 线程启动\n");
    while (g_running) {
        device_task_t *task = (device_task_t *)task_queue_pop(g_ptz_queue, 500);
        if (!task) continue;

        printf("[WORKER-PTZ] 执行 cmd=%d\n", task->cmd_id);

        cJSON *resp_json = NULL;
        int    ret = device_ptz_execute(&task->cmd, &resp_json);

        if (resp_json) {
            char *s = msg_to_string(resp_json);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        } else if (ret != 0) {
            cJSON *err = msg_build_response(ERR_DEVICE_COMM, "command failed");
            char  *s   = msg_to_string(err);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        }

        config_manager_update_after_cmd(task->cmd_id);
        device_task_destroy(task);
    }
    printf("[WORKER-PTZ] 线程退出\n");
    return NULL;
}

/* ---- LED Worker ---- */
static void *led_worker_thread(void *arg)
{
    (void)arg;
    printf("[WORKER-LED] 线程启动\n");
    while (g_running) {
        device_task_t *task = (device_task_t *)task_queue_pop(g_led_queue, 500);
        if (!task) continue;

        printf("[WORKER-LED] 执行 cmd=%d\n", task->cmd_id);

        cJSON *resp_json = NULL;
        int    ret = device_led_execute(&task->cmd, &resp_json);

        if (resp_json) {
            char *s = msg_to_string(resp_json);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        } else if (ret != 0) {
            cJSON *err = msg_build_response(ERR_DEVICE_COMM, "command failed");
            char  *s   = msg_to_string(err);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        }

        config_manager_update_after_cmd(task->cmd_id);
        device_task_destroy(task);
    }
    printf("[WORKER-LED] 线程退出\n");
    return NULL;
}

/* ---- Speaker Worker ---- */
static void *spk_worker_thread(void *arg)
{
    (void)arg;
    printf("[WORKER-SPK] 线程启动\n");
    while (g_running) {
        device_task_t *task = (device_task_t *)task_queue_pop(g_spk_queue, 500);
        if (!task) continue;

        printf("[WORKER-SPK] 执行 cmd=%d\n", task->cmd_id);

        cJSON *resp_json = NULL;
        int    ret = device_speaker_execute(&task->cmd, &resp_json);

        if (resp_json) {
            char *s = msg_to_string(resp_json);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        } else if (ret != 0) {
            cJSON *err = msg_build_response(ERR_DEVICE_COMM, "command failed");
            char  *s   = msg_to_string(err);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        }

        config_manager_update_after_cmd(task->cmd_id);
        device_task_destroy(task);
    }
    printf("[WORKER-SPK] 线程退出\n");
    return NULL;
}

/* ---- Alarm Worker ---- */
static void *alarm_worker_thread(void *arg)
{
    (void)arg;
    printf("[WORKER-ALARM] 线程启动\n");
    while (g_running) {
        device_task_t *task = (device_task_t *)task_queue_pop(g_alarm_queue, 500);
        if (!task) continue;

        printf("[WORKER-ALARM] 执行 cmd=%d\n", task->cmd_id);

        cJSON *resp_json = NULL;
        int    ret = device_alarm_execute(&task->cmd, &resp_json);

        if (resp_json) {
            char *s = msg_to_string(resp_json);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        } else if (ret != 0) {
            cJSON *err = msg_build_response(ERR_DEVICE_COMM, "command failed");
            char  *s   = msg_to_string(err);
            if (s) ws_send_queue_enqueue(g_send_queue, s);
        }

        config_manager_update_after_cmd(task->cmd_id);
        device_task_destroy(task);
    }
    printf("[WORKER-ALARM] 线程退出\n");
    return NULL;
}

/* ---- 初始化所有 worker 线程和队列 ---- */
static int init_all_workers(void)
{
    g_ptz_queue   = task_queue_create(16);
    g_led_queue   = task_queue_create(16);
    g_spk_queue   = task_queue_create(16);
    g_alarm_queue = task_queue_create(16);
    if (!g_ptz_queue || !g_led_queue || !g_spk_queue || !g_alarm_queue) {
        printf("[MAIN] 创建 task_queue 失败\n");
        return -1;
    }

    g_send_queue = ws_send_queue_init(g_ws_client);
    if (!g_send_queue) {
        printf("[MAIN] 创建 send_queue 失败\n");
        return -1;
    }

    if (pthread_create(&g_ptz_thread,   NULL, ptz_worker_thread,   NULL) != 0) return -1;
    if (pthread_create(&g_led_thread,   NULL, led_worker_thread,   NULL) != 0) return -1;
    if (pthread_create(&g_spk_thread,   NULL, spk_worker_thread,   NULL) != 0) return -1;
    if (pthread_create(&g_alarm_thread, NULL, alarm_worker_thread, NULL) != 0) return -1;

    g_workers_inited = true;
    printf("[MAIN] 所有 Worker 线程已启动 (PTZ/LED/SPK/ALARM)\n");
    return 0;
}

/* ======================================================================== */
/*  连接状态变化回调（mongoose 线程调用，仅设置标志）                             */
/* ======================================================================== */

static void on_ws_state_change(bool connected, void *user_data)
{
    (void)user_data;

    if (connected) {
        /* 上升沿：重连成功，通知 main_loop 发起重认证 */
        printf("[MAIN] WS 连接建立（回调）\n");
        if (!g_ws_was_connected) {
            g_need_reauth = true;
        }
    } else {
        /* 下降沿：连接断开 */
        printf("[MAIN] WS 连接断开（回调）\n");
        config_manager_set_connected(false);
    }
    g_ws_was_connected = connected;
}

/* ======================================================================== */
/*  设备认证（cmd=11）                                                        */
/* ======================================================================== */

static int do_authenticate(void)
{
    cJSON *auth_json = msg_build_auth_request();
    char  *auth_str  = msg_to_string(auth_json);
    if (!auth_str) {
        printf("[MAIN] 认证消息构建失败\n");
        return -1;
    }

    printf("[MAIN] 发送认证请求: %s\n", auth_str);

    if (!ws_client_send(g_ws_client, auth_str)) {
        printf("[MAIN] 认证请求发送失败\n");
        free(auth_str);
        return -1;
    }
    free(auth_str);

    printf("[MAIN] 等待认证回复...\n");
    for (int i = 0; i < 50 && g_running; i++) {
        char *raw = ring_buffer_pop(g_rx_buffer);
        if (raw) {
            printf("[MAIN] 收到认证回复: %s\n", raw);

            cmd_t cmd;
            if (cmd_parser_parse(raw, &cmd)) {
                if (cmd.cmd_id == CMD_AUTH) {
                    cJSON *token_item = cJSON_GetObjectItem(cmd.root, "token");
                    if (cJSON_IsString(token_item)) {
                        printf("[MAIN] 认证成功, token=%s\n",
                               cJSON_GetStringValue(token_item));
                        config_manager_set_auth_token(
                            cJSON_GetStringValue(token_item));
                        /* 同步更新 ws_client 内部 token，确保重连时使用最新值 */
                        ws_client_set_auth_token(g_ws_client,
                            cJSON_GetStringValue(token_item));
                    }
                    cJSON *result_item = cJSON_GetObjectItem(cmd.root, "result");
                    int result = cJSON_IsNumber(result_item)
                                 ? result_item->valueint : -1;
                    cmd_parser_free(&cmd);
                    free(raw);
                    return (result == 0) ? 0 : -1;
                }
                printf("[MAIN] 认证期间收到非认证消息(cmd=%d)，丢弃\n",
                       cmd.cmd_id);
                cmd_parser_free(&cmd);
            }
            free(raw);
        }
        usleep(100000);
    }

    printf("[MAIN] 认证超时\n");
    return -1;
}

/* ======================================================================== */
/*  判断消息是否为服务端事件确认（ack）                                          */
/* ======================================================================== */

/**
 * @brief   服务端在收到客户端上报的 event 消息后，会回复对应的 ack 消息
 *          （如 system_heartbeat_ack、device_status_ack 等）。
 *
 *          这些 ack 消息特征：有 "event" 字段，没有 "cmd" 字段。
 *          客户端应记录日志后静默处理，不需要走 cmd 分发，更不应该回复错误。
 *
 * @param   raw_msg 原始 JSON 字符串
 * @return  true=是 ack 消息（应跳过 cmd 分发），false=不是
 */
static bool is_server_ack(const char *raw_msg)
{
    /* 快速预检：包含 "event" 字符串即可进一步解析 */
    if (!strstr(raw_msg, "\"event\"")) {
        return false;
    }

    cJSON *root = cJSON_Parse(raw_msg);
    if (!root) return false;

    cJSON *event_item = cJSON_GetObjectItem(root, "event");
    bool is_ack = (cJSON_IsString(event_item) && !cJSON_HasObjectItem(root, "cmd"));

    if (is_ack) {
        printf("[MAIN] 服务端确认: event=%s\n",
               cJSON_GetStringValue(event_item));
    }

    cJSON_Delete(root);
    return is_ack;
}

/* ======================================================================== */
/*  主循环                                                                    */
/* ======================================================================== */

static void main_loop(void)
{
    printf("[MAIN] 进入主循环（多线程外设控制模式）\n");

    while (g_running) {

        /* ---- 重连后重新鉴权 ---- */
        if (g_need_reauth) {
            g_need_reauth = false;
            printf("[MAIN] 检测到重连，开始重新鉴权...\n");
            config_manager_set_connected(true);
            if (do_authenticate() != 0) {
                printf("[MAIN] 重连后鉴权失败（服务器可能尚未就绪）\n");
            }
        }

        /* ---- 取消息 ---- */
        char *raw_msg = ring_buffer_pop(g_rx_buffer);
        if (!raw_msg) {
            usleep(MAIN_LOOP_SLEEP_US);
            continue;
        }

        printf("[MAIN] 收到: %s\n", raw_msg);

        /* ---- 服务端 ack 消息：仅记录日志，跳过 ---- */
        if (is_server_ack(raw_msg)) {
            free(raw_msg);
            continue;
        }

        /* ---- 解析 cmd ---- */
        cmd_t cmd;
        if (!cmd_parser_parse(raw_msg, &cmd)) {
            printf("[MAIN] 解析失败\n");
            free(raw_msg);
            continue;
        }

        /* cmd_id 无效的消息（如氛围灯）走特殊分发 */
        if (cmd.cmd_id < 0) {
            cJSON *state_item = cJSON_GetObjectItem(cmd.root, "state");
            cJSON *type_item  = cJSON_GetObjectItem(cmd.root, "type");
            if (cJSON_IsString(state_item) || cJSON_IsString(type_item)) {
                /* 氛围灯：主线程内联执行 */
                printf("[MAIN] 氛围灯指令，主线程执行\n");
                cJSON *resp = NULL;
                int    ret  = cmd_dispatch_execute(&cmd, &resp);
                if (ret != 0 && !resp) {
                    resp = msg_build_cmd_response(ERR_DEVICE_COMM, cmd.cmd_id,
                                                  "氛围灯命令执行失败");
                }
                config_manager_update_mood();
                if (resp) {
                    char *s = msg_to_string(resp);
                    if (s) {
                        ws_send_queue_enqueue(g_send_queue, s);
                    } else {
                        printf("[MAIN] 氛围灯回复序列化失败\n");
                    }
                }
            } else {
                printf("[MAIN] 消息无有效 cmd，跳过: %s\n", raw_msg);
            }
            cmd_parser_free(&cmd);
            free(raw_msg);
            continue;
        }

        /* ---- 按设备类型路由 ---- */
        device_type_t dev = cmd_dispatch_get_device_type(cmd.cmd_id);

        switch (dev) {
        case DEVICE_PTZ:
        case DEVICE_LED:
        case DEVICE_SPEAKER:
        case DEVICE_ALARM: {
            /*
             * 云台特殊处理：收到移动/归位/巡航/扫描指令时，
             * 先中断正在执行的预置位设置（如果存在），
             * 让 worker 尽快响应新指令而非排队等待。
             */
            if (dev == DEVICE_PTZ &&
                (cmd.cmd_id == 101 || cmd.cmd_id == 102 ||
                 cmd.cmd_id == 104 || cmd.cmd_id == 105)) {
                device_ptz_abort_preset();
            }

            /* 投递到对应 worker 队列（非阻塞） */
            task_queue_t *q =
                (dev == DEVICE_PTZ)     ? g_ptz_queue   :
                (dev == DEVICE_LED)     ? g_led_queue   :
                (dev == DEVICE_SPEAKER) ? g_spk_queue   :
                                          g_alarm_queue;

            device_task_t *task = device_task_create(&cmd);
            if (!task || !task_queue_push(q, task)) {
                printf("[MAIN] 队列满，cmd=%d 拒绝\n", cmd.cmd_id);
                cJSON *err = msg_build_response(ERR_DEVICE_COMM, "task queue full");
                char  *s   = msg_to_string(err);
                if (s) ws_send_queue_enqueue(g_send_queue, s);
                if (task) device_task_destroy(task);
            }
            break;
        }

        case DEVICE_SYSTEM:
        default: {
            /* 系统命令（601-607 等）和未知命令：主线程内联执行 */
            cJSON *resp = NULL;
            int    ret  = cmd_dispatch_execute(&cmd, &resp);
            config_manager_update_after_cmd(cmd.cmd_id);

            if (resp) {
                char *s = msg_to_string(resp);
                if (s) ws_send_queue_enqueue(g_send_queue, s);
            } else if (ret != 0) {
                cJSON *err = msg_build_cmd_response(ERR_DEVICE_COMM, cmd.cmd_id,
                                                    "command failed");
                char  *s   = msg_to_string(err);
                if (s) ws_send_queue_enqueue(g_send_queue, s);
            }
            break;
        }
        }

        cmd_parser_free(&cmd);
        free(raw_msg);
    }

    printf("[MAIN] 主循环退出\n");
}

/* ======================================================================== */
/*  程序入口                                                                  */
/* ======================================================================== */

int main(void)
{
    printf("============================================\n");
    printf("  边检机器人控制板 %s\n", CTRL_BOARD_FW_VERSION);
    printf("  型号: %s\n", CTRL_BOARD_MODEL);
    printf("============================================\n\n");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. 初始化设备（硬件初始化 + 设置默认内存状态） */
    if (init_all_devices() != 0) {
        printf("[MAIN] 设备初始化失败\n");
        cleanup();
        return EXIT_FAILURE;
    }

    /* 2. 配置管理器：加载已有 JSON 配置文件到内存缓存 */
    if (config_manager_init("./config") != 0) {
        printf("[MAIN] 配置管理器初始化失败\n");
        cleanup();
        return EXIT_FAILURE;
    }
    printf("[MAIN] 当前运行版本: %s\n", config_manager_get_fw_version());

    /*
     * 2.5 启动看门狗守护进程
     *
     * 看门狗独立于主进程运行（fork + 双 setsid），每 30 秒检测主进程存活状态。
     * 若主进程死亡，检查 upgrade_pending.json 决定是否替换二进制后重启。
     *
     * 检查 DEVICE_WATCHDOG_ACTIVE 环境变量：若已设置（由看门狗重启时设置），
     * 跳过 fork 新看门狗，避免链条式累积。
     *
     * PID 文件必须先于看门狗写入，确保看门狗启动后能立即读取到有效的 PID。
     */
    watchdog_write_pid();
    if (getenv("DEVICE_WATCHDOG_ACTIVE") == NULL) {
        watchdog_daemon_start();
    } else {
        printf("[MAIN] 由看门狗重启启动，跳过看门狗 fork\n");
    }

    /* 3. 将配置文件中保存的状态恢复到各设备模块的内存中
     *    必须在 sync 之前执行，否则 sync 会用默认值覆盖文件 */
    config_manager_restore_device_state();

    /* 3.5 从 preset_positions.yaml 恢复预置位到 PTZ 模块内存 */
    config_manager_load_presets();

    /* 4. 从设备模块采集实际状态（含硬件查询），更新并落盘配置文件
     *    - PTZ: 查询硬件实际角度，覆盖文件中的旧值
     *    - LED: 读取已恢复的内存状态，保持不变
     *    - 音箱: 读取已恢复的内存状态，保持不变
     *    - 警灯: 读取硬件 GPIO 状态，覆盖文件
     *    - 氛围灯: 读取已恢复的内存状态，保持不变 */
    config_manager_sync_device_status();

    /* 5. 创建环形缓冲区 */
    g_rx_buffer = ring_buffer_create(RING_BUFFER_CAPACITY);
    if (!g_rx_buffer) {
        printf("[MAIN] ring_buffer 创建失败\n");
        cleanup();
        return EXIT_FAILURE;
    }

    /* 6. 启动 WebSocket */
    ws_client_config_t ws_cfg = {
        .server_url      = WS_SERVER_URL,
        .auth_token      = WS_AUTH_TOKEN,
        .rx_buffer       = g_rx_buffer,
        .reconnect_sec   = WS_RECONNECT_INTERVAL,
        .ping_sec        = WS_PING_INTERVAL,
        .on_state_change = on_ws_state_change,
        .user_data       = NULL,
    };

    g_ws_client = ws_client_init(&ws_cfg);
    if (!g_ws_client) {
        printf("[MAIN] WS 启动失败\n");
        cleanup();
        return EXIT_FAILURE;
    }

    /* 等待连接建立 */
    printf("[MAIN] 等待 WebSocket 连接...\n");
    for (int i = 0; i < 30 && g_running; i++) {
        if (ws_client_is_connected(g_ws_client)) break;
        usleep(200000);
    }

    if (!ws_client_is_connected(g_ws_client)) {
        printf("[MAIN] WS 连接超时\n");
        cleanup();
        return EXIT_FAILURE;
    }
    config_manager_set_connected(true);

    /* 7. 设备认证 */
    if (do_authenticate() != 0) {
        printf("[MAIN] 认证失败\n");
        cleanup();
        return EXIT_FAILURE;
    }

    /* 7.5 与国家授时中心 NTP 对时（认证成功说明网络通，此时仅主线程，fork 安全） */
    device_system_ntp_sync();

    /* 8. 初始化 send_queue 和 worker 线程 */
    if (init_all_workers() != 0) {
        printf("[MAIN] Worker 线程初始化失败\n");
        cleanup();
        return EXIT_FAILURE;
    }

    /* 9. 启动心跳（传入 send_queue） */
    g_heartbeat = heartbeat_start(g_ws_client, g_send_queue, HEARTBEAT_INTERVAL);
    if (!g_heartbeat) {
        printf("[MAIN] 心跳启动失败\n");
        cleanup();
        return EXIT_FAILURE;
    }

    /* 9.5 发送升级完成响应（若存在 upgrade_pending.json） */
    config_manager_send_pending_upgrade_response(g_send_queue);

    /* 10. 主循环 */
    main_loop();

    cleanup();
    return EXIT_SUCCESS;
}