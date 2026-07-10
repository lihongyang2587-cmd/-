/**
 * @file    device_ptz.h
 * @brief   云台控制模块（V3.1 — 绝对角度定位）
 *
 *          cmd=101: 绝对定位  dir=1/2时angle=绝对水平角度(0~350°)
 *                              dir=3时angle=正俯仰角, dir=4时angle=负俯仰角
 *                              speed=0~10, stop=1时停止
 *          cmd=102: 绝对角度  horizontalAngle/verticalAngle, speed
 *          cmd=103: 预置位    groupNum=1|2, groupData[]
 *          cmd=104: 巡航      groupNum, speed
 *          cmd=105: 扫描      scanDir=1水平/2垂直, speed
 *
 *          所有指令均为非阻塞模式：发送 Pelco-D 帧后立即返回，
 *          不轮询等待到位，避免 WebSocket 主循环被卡死。
 *
 *          V3.1 变更：
 *          - cmd=101 angle 从相对偏移改为绝对目标角度
 *          - device_ptz_query_position() 增加自主模式保护
 *          - cmd=601 不再阻塞主线程查询硬件，使用缓存值
 */

#ifndef DEVICE_PTZ_H
#define DEVICE_PTZ_H

#include "cmd_parser.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  角度范围（对齐 API 规格书）                                                   */
/* ======================================================================== */
#define PTZ_H_ANGLE_MIN       0
#define PTZ_H_ANGLE_MAX     350
#define PTZ_V_ANGLE_MIN    -32
#define PTZ_V_ANGLE_MAX      23

/* ======================================================================== */
/*  预置位数据结构                                                              */
/* ======================================================================== */
#define PTZ_PRESET_MAX_POINTS   16
#define PTZ_PRESET_GROUP_COUNT   2

typedef struct {
    int  index;
    int  hor_angle;
    int  ver_angle;
    int  success;       /**< 1=写入硬件成功, 0=失败（仅 cmd=103 设置时更新） */
} ptz_preset_t;

typedef struct {
    int           count;
    ptz_preset_t  points[PTZ_PRESET_MAX_POINTS];
} ptz_preset_group_t;

/** 状态变更回调函数类型 */
typedef void (*ptz_status_cb_t)(void);

/* ======================================================================== */
/*  公开接口                                                                  */
/* ======================================================================== */

int    device_ptz_init(const char *uart_dev, int baudrate);
void   device_ptz_set_status_cb(ptz_status_cb_t cb);
int    device_ptz_poll_status_changed(void);
int    device_ptz_execute(const cmd_t *cmd, cJSON **resp);
cJSON *device_ptz_get_status(void);
int    device_ptz_query_position(void);
int    device_ptz_get_fail_status(void);
void   device_ptz_deinit(void);

/**
 * @brief   手动设置水平扫描限位（阻塞，需等待云台到位）
 *
 *          首次水平扫描 (scanDir=1) 时会自动设置默认限位 (0°~350°)，
 *          如需自定义限位范围，可在扫描前调用本函数。
 *
 * @param left_pan   左限位水平角度（度）
 * @param right_pan  右限位水平角度（度）
 * @return 0 成功，-1 失败
 */
int    device_ptz_set_scan_limits(double left_pan, double right_pan);

/**
 * @brief   获取指定预置位组数据（从内存缓存中读取）
 * @param   group_num  组号 1 或 2
 * @param   group      输出，预置位组数据
 * @return  0 成功，-1 失败（参数无效或组为空）
 */
int    device_ptz_get_preset_group(int group_num, ptz_preset_group_t *group);

/**
 * @brief   用 JSON 数据填充预置位组内存缓存（从配置文件加载时使用）
 * @param   group_num  组号 1 或 2
 * @param   json      预置位组 JSON 对象
 * @return  0 成功，-1 失败
 */
int    device_ptz_load_preset_group(int group_num, cJSON *json);

/**
 * @brief   将预置位组数据导出为 JSON（写入配置文件时使用）
 * @param   group_num  组号 1 或 2
 * @return  JSON 对象，调用者负责 cJSON_Delete；无数据时返回空对象
 */
cJSON *device_ptz_export_preset_group(int group_num);

/**
 * @brief   中断正在执行的预置位设置（由主线程在收到 stop 指令时调用）
 *
 *          设置 g_preset_abort 标志，handle_preset() 在下一次轮询间隙
 *          检测到后立即停止硬件并返回。本函数不阻塞，不访问串口。
 */
void device_ptz_abort_preset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_PTZ_H */