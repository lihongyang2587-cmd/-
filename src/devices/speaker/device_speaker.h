/**
 * @file    device_speaker.h
 * @brief   音箱控制模块（USB 音频 — V2.6 完整实现）
 *
 *          cmd=301: 播放控制 (audioIndex/play/pause/stop/volume/playType/playData)
 *          cmd=302: 设置音频列表 (downUrl/audioData) + 下载
 *          cmd=303: 获取音频列表
 */

#ifndef DEVICE_SPEAKER_H
#define DEVICE_SPEAKER_H

#include "cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态变更回调函数类型（可选，当前使用标志位轮询） */
typedef void (*speaker_status_cb_t)(void);

/**
 * @brief   初始化音箱模块
 * @return  0 成功
 */
int device_speaker_init(void);

/**
 * @brief   注册状态变更回调（可选）
 * @param   cb  回调函数指针，传 NULL 则取消注册
 */
void device_speaker_set_status_cb(speaker_status_cb_t cb);

/**
 * @brief   查询音箱状态是否发生变更
 *          调用后自动清除标志位。主循环每轮调用，返回 1 时应立即推送 device_status。
 * @return  1=有变更需要上报，0=无变更
 */
int device_speaker_poll_status_changed(void);

/**
 * @brief   执行音箱命令
 * @param   cmd     已解析的命令
 * @param   resp    输出，回复 JSON（调用者负责 cJSON_Delete）
 * @return  0 成功，非 0 失败
 */
int device_speaker_execute(const cmd_t *cmd, cJSON **resp);

/**
 * @brief   获取当前音箱状态
 * @return  cJSON 对象（调用者负责 cJSON_Delete）
 */
cJSON *device_speaker_get_status(void);
int    device_speaker_get_fail_status(void);
void   device_speaker_apply_config(int volume);

/**
 * @brief   释放音箱模块资源
 */
void device_speaker_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_SPEAKER_H */