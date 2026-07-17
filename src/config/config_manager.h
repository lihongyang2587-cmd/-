/**
 * @file    config_manager.h
 * @brief   运行时配置管理器
 *
 *          将外设参数/状态和服务器配置持久化为 JSON 文件，按类别分目录存放。
 *
 *          启动流程：
 *          1. config_manager_init()      — 创建目录，加载已有配置或写默认值
 *          2. config_manager_sync_device_status() — 从设备模块采集实际状态，更新文件
 *
 *          运行时：
 *          - 每条指令执行完成后调用 config_manager_update_after_cmd() 增量更新
 *          - 认证/连接状态变更通过 setter 写入 server.json
 *
 *          文件清单（config/ 目录下）：
 *            server.json   ptz.json   led.json   speaker.json
 *            alarm.json    mood.json  system.json
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include "cJSON.h"

/* 前向声明（避免引入 ws_send_queue.h 的传递依赖） */
typedef struct ws_send_queue ws_send_queue_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

/**
 * @brief   初始化配置管理器
 * @param   config_dir  配置目录路径，如 "./config"
 * @return  0 成功，-1 失败（目录创建失败等）
 *
 *          依次处理 7 个类别：文件存在则加载到内存缓存，不存在则创建默认文件。
 */
int  config_manager_init(const char *config_dir);

/**
 * @brief   将已加载的配置文件内容恢复到各设备模块的内存状态中
 *
 *          必须在 config_manager_init() 之后、
 *          config_manager_sync_device_status() 之前调用。
 *
 *          恢复的设备类别：LED（字幕/亮度/模式）、音箱（音量）、氛围灯（state/type）。
 *          不恢复：PTZ（角度由硬件传感器决定）、警灯（安全原因，重启后应为关闭）。
 */
void config_manager_restore_device_state(void);

/**
 * @brief   从各设备模块采集当前状态，更新并保存设备配置文件
 *
 *          调用 device_ptz/led/speaker/alarm/mood 的 get_status()，
 *          将返回的 JSON 合并到对应类别缓存中并写入磁盘。
 *          同步的类别：ptz, led, speaker, alarm, mood（不含 server 和 system）。
 */
void config_manager_sync_device_status(void);

/**
 * @brief   指令执行完成后，增量更新对应的配置文件
 * @param   cmd_id  已执行的命令 ID（如 101, 201, 401, 601…）
 *
 *          映射关系：
 *          - 101~105 → ptz.json
 *          - 201~202 → led.json
 *          - 301~303 → speaker.json
 *          - 401     → alarm.json
 *          - 601     → 全部设备 config（ptz/led/speaker/alarm/mood）
 *          - 605     → server.json（serverUrl 可能变更）
 *          - 氛围灯(state) → mood.json（由调用方传入负值触发）
 */
void config_manager_update_after_cmd(int cmd_id);

/**
 * @brief   快速刷新所有设备配置缓存（trylock，无硬件 I/O）
 *
 *          从各设备模块采集当前内存状态（device_xxx_get_status()），
 *          合并到配置缓存并写入磁盘。不涉及串口 I/O 或阻塞等待，
 *          可在心跳线程等非命令路径中安全调用。
 *
 *          调用场景：
 *          - cmd=601 状态采集前（确保响应反映最新状态）
 *          - 心跳上报前（确保 device_status 反映最新状态）
 *          - 异步状态变化后（如音箱播放结束）
 *
 *          调用者无需持有任何锁，内部自行处理 g_cfg_mutex。
 */
void config_manager_refresh_all(void);

/**
 * @brief   检查并发送升级完成响应（新程序启动后调用）
 *
 *          若 upgrade_pending.json 存在，读取其中的 token/toVer 等字段，
 *          构建 cmd=604 成功响应通过 send_queue 发送，然后删除文件。
 *
 *          应在 WebSocket 连接建立并认证成功后调用。
 *
 * @param   send_queue  WS 发送队列（用于发送响应 JSON）
 */
void config_manager_send_pending_upgrade_response(ws_send_queue_t *send_queue);

/**
 * @brief   更新氛围灯配置（无 cmd_id，由调用方主动调用）
 */
void config_manager_update_mood(void);

/**
 * @brief   将预置位组数据写入 preset_positions.yaml（cmd=103 成功后调用）
 */
void config_manager_save_presets(void);

/**
 * @brief   从 preset_positions.yaml 读取预置位数据恢复到内存（启动时调用）
 */
void config_manager_load_presets(void);

/**
 * @brief   删除 preset_positions.yaml（cmd=103 预置位设置前调用）
 *
 *          与 gimbal_clear_presets() 配套使用，确保文件与硬件同步清除。
 */
void config_manager_clear_presets(void);

/* ---- 服务器配置更新 ---- */

void config_manager_set_auth_token(const char *token);
void config_manager_set_connected(bool connected);

/**
 * @brief   读取已持久化的 authToken
 * @param   dest  输出缓冲区
 * @param   size  缓冲区大小（建议 >= 256）
 * @return  true 成功读取到非空 token，false 无 token 或未初始化
 */
bool config_manager_get_auth_token(char *dest, size_t size);

/* ---- 设备状态缓存读取（不访问硬件，从 g_cache[] 深拷贝） ---- */

/**
 * @brief   从内存缓存获取云台状态（深拷贝，不访问硬件）
 *
 *          返回 g_cache[CAT_PTZ] 的深拷贝副本。缓存由各 worker 线程在每次
 *          命令执行后通过 config_manager_update_after_cmd() 更新，
 *          启动时通过 config_manager_sync_device_status() 初始化。
 *
 * @return  cJSON 对象，调用者负责 cJSON_Delete；未初始化或缓存为空返回 NULL
 */
cJSON *config_manager_get_ptz_status(void);

/**
 * @brief   从内存缓存获取 LED 字幕屏状态（深拷贝，不访问硬件）
 * @return  cJSON 对象，调用者负责 cJSON_Delete；未初始化或缓存为空返回 NULL
 */
cJSON *config_manager_get_led_status(void);

/**
 * @brief   从内存缓存获取音箱状态（深拷贝，不访问硬件）
 * @return  cJSON 对象，调用者负责 cJSON_Delete；未初始化或缓存为空返回 NULL
 */
cJSON *config_manager_get_speaker_status(void);

/**
 * @brief   从内存缓存获取警灯状态（深拷贝，不访问硬件）
 * @return  cJSON 对象，调用者负责 cJSON_Delete；未初始化或缓存为空返回 NULL
 */
cJSON *config_manager_get_alarm_status(void);

/**
 * @brief   从内存缓存获取氛围灯状态（深拷贝，不访问硬件）
 * @return  cJSON 对象，调用者负责 cJSON_Delete；未初始化或缓存为空返回 NULL
 */
cJSON *config_manager_get_mood_status(void);

/**
 * @brief   释放所有缓存并清理资源
 */
void config_manager_deinit(void);

/**
 * @brief   获取当前固件版本号（权威来源为编译版本 CTRL_BOARD_FW_VERSION）
 *
 *          config_manager_init 之后：返回 system.json 中的 fwVersion，
 *          已由 init 的自动修正逻辑同步为 CTRL_BOARD_FW_VERSION。
 *
 *          config_manager_init 之前或初始化失败：回退到 CTRL_BOARD_FW_VERSION，
 *          确保调用者在任何时机都能安全拿到一个有效字符串。
 *
 *          注意：服务端下发的 mainVer 仅用于升级前版本比对，
 *          不参与 fwVersion 的确定。
 *
 * @return  版本号字符串（如 "1.0.0"），指针指向内部缓存，调用者不应释放。
 */
const char *config_manager_get_fw_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MANAGER_H */
