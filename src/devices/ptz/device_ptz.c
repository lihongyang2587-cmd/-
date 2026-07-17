/**
 * @file    device_ptz.c
 * @brief   云台控制实现（V3.2 — dir=0 绝对定位 + dir≠0 相对定位）
 *
 *          V3.2 变更：
 *          - [重构] cmd=101 handle_move():
 *            dir=0 时 angle=绝对水平目标角度（新增），不查询硬件。
 *            dir=1~4 时 angle=相对当前位置的偏移量，执行前查询一次硬件角度。
 *          V3.1 变更：
 *          - [修复] device_ptz_query_position() 增加 g_autonomous 检查，
 *            扫描/巡航期间跳过硬件查询避免长时间阻塞。
 *
 *          V3.0 变更：
 *          - [重构] 移除所有主动硬件角度查询。扫描期间 RS-485 半双工总线上
 *            硬件持续发送位置上报帧（每帧 ~29ms）。
 *            此后角度仅在服务端定位指令执行后更新为指令目标值。
 *          - [重构] hardware_force_stop() / stop_autonomous_mode()
 *            统一 drain+move+stop 序列退出自主模式。
 *          - [重构] device_ptz_get_status() 加锁读取缓存值，
 *            不再查询硬件，新增 moving/autonomous 字段。
 *          - [修复] gimbal_drain_rx() 增加最大 140 字节限制防止死循环。
 */

#include "device_ptz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>

#include "gimbal.h"
#include "msg_builder.h"
#include "config.h"
#include "config_manager.h"

/* ======================================================================== */
/*  错误码                                                                    */
/* ======================================================================== */

#ifndef ERR_SUCCESS
#define ERR_SUCCESS         0
#endif
#ifndef ERR_PARAM_INVALID
#define ERR_PARAM_INVALID   1001
#endif
#ifndef ERR_NOT_SUPPORTED
#define ERR_NOT_SUPPORTED   1003
#endif
#ifndef ERR_TIMEOUT
#define ERR_TIMEOUT         1004
#endif
#ifndef ERR_DEVICE_COMM
#define ERR_DEVICE_COMM     3003
#endif

/* ======================================================================== */
/*  内部常量                                                                   */
/* ======================================================================== */

#define SPEED_MIN       0
#define SPEED_MAX       10
#define SPEED_DEFAULT   5

/** Pelco-D 协议速度码范围 0x00~0x3F */
#define SPEED_CODE_MAX  0x3F

/** 线扫速度档位范围（5~13 共 9 档） */
#define SCAN_SPEED_MIN  5
#define SCAN_SPEED_MAX  13

/** 云台指令间隔（对齐 gimbal.c 内部时序） */
#ifndef GIMBAL_SYNC_DELAY_US
#define GIMBAL_SYNC_DELAY_US            100000   /* 100ms */
#endif

#ifndef GIMBAL_POSITION_SETTLE_DELAY_US
#define GIMBAL_POSITION_SETTLE_DELAY_US  500000  /* 500ms */
#endif

/** 位置轮询参数（对齐 gimbal.c: GIMBAL_POSITION_TOLERANCE_DEG / GIMBAL_POSITION_TIMEOUT_MS） */
#ifndef PTZ_POSITION_TOLERANCE_DEG
#define PTZ_POSITION_TOLERANCE_DEG       0.1
#endif
#ifndef PTZ_POSITION_TIMEOUT_MS
#define PTZ_POSITION_TIMEOUT_MS          120000
#endif

/* ======================================================================== */
/*  模块内部状态                                                               */
/* ======================================================================== */

static gimbal_t g_gimbal;

/*
 * 全局互斥锁，保护对 g_gimbal 的串口访问。
 * 心跳线程（msg_build_device_status → device_ptz_get_status）和主循环线程
 * （device_ptz_execute / stop_autonomous_mode）会并发访问同一个串口，
 * 不加锁会导致帧交错、双方同时超时。
 */
static pthread_mutex_t g_gimbal_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_current_h_angle = 0;
static int g_current_v_angle = 0;
static int g_moving          = 0;
static int g_active          = 0;
static int g_current_speed   = 0;      /* 当前速度（cmd=101 speed 参数） */
static int g_fail_status     = 0;      /* 0=正常, 1=不在线, 2=响应异常 */

/** 巡航/扫描期间为 1，跳过硬件角度查询，避免与硬件主动上报帧冲突 */
static int g_autonomous      = 0;

/** 扫描限位已设置标志 */
static int g_left_limit_set  = 0;
static int g_right_limit_set = 0;
static int g_active_scan      = 0;

static ptz_preset_group_t g_preset_groups[PTZ_PRESET_GROUP_COUNT];

/** 预置位设置中断标志，由主线程在收到 stop 指令时设置 */
static volatile int g_preset_abort = 0;

static volatile int g_status_changed = 0;
static ptz_status_cb_t g_status_changed_cb = NULL;

/* ======================================================================== */
/*  内部工具函数                                                               */
/* ======================================================================== */

static void notify_status_changed(void)
{
    g_status_changed = 1;
    if (g_status_changed_cb) {
        g_status_changed_cb();
    }
}

/**
 * @brief   无条件向硬件发送停止序列（不阻塞，不查询验证）
 *
 *          云台在自主模式（自检/扫描/巡航）下会忽略标准 STOP 帧。
 *          先 drain RX 清空硬件上报帧、等待 RS-485 总线空闲，
 *          再用手动运动指令抢占总线退出自主模式后停止。
 *
 *          调用者必须持有 g_gimbal_mutex。
 */
static void hardware_force_stop(void)
{
    printf("[PTZ] 发送硬件强制停止序列 (清总线+抢占+停止)\n");

    /* 清空 RX 缓冲，等待总线空闲，避免 RS-485 半双工碰撞 */
    gimbal_drain_rx(&g_gimbal);

    /* 手动运动指令退出自主模式，再停止 */
    gimbal_move(&g_gimbal, GIMBAL_DIR_RIGHT, 0x01);
    usleep(50000);
    gimbal_stop(&g_gimbal);

    g_moving      = 0;
    g_autonomous  = 0;
    g_active_scan = 0;

    /*
     * 停止后查询硬件实际角度，更新缓存。
     * 此时硬件已退出自主模式，RS-485 总线空闲，查询可以正常收到响应。
     */
    {
        double hw_pan = 0.0, hw_tilt = 0.0;
        if (gimbal_query_pan(&g_gimbal, &hw_pan) == 0) {
            g_current_h_angle = (int)(hw_pan + 0.5);
        }
        usleep(50000);
        if (gimbal_query_tilt(&g_gimbal, &hw_tilt) == 0) {
            g_current_v_angle = (int)(hw_tilt + 0.5);
        }
        printf("[PTZ] 停止后角度: h=%d° v=%d°\n",
               g_current_h_angle, g_current_v_angle);
    }

    printf("[PTZ] 硬件强制停止序列完成\n");
}

/**
 * @brief   API speed 档位 (0~10) → Pelco-D 协议速度码 (0x00~0x3F)
 */
static uint8_t speed_to_code(int speed)
{
    if (speed <= SPEED_MIN) return 0;
    if (speed >= SPEED_MAX) return SPEED_CODE_MAX;
    return (uint8_t)((speed * SPEED_CODE_MAX) / SPEED_MAX);
}

/**
 * @brief   API speed 档位 (0~10) → 线扫速度档位 (5~13)
 */
static uint8_t speed_to_scan_code(int speed)
{
    if (speed <= SPEED_MIN) return SCAN_SPEED_MIN;
    if (speed >= SPEED_MAX) return SCAN_SPEED_MAX;
    return (uint8_t)(SCAN_SPEED_MIN
                     + (speed * (SCAN_SPEED_MAX - SCAN_SPEED_MIN)) / SPEED_MAX);
}

/**
 * @brief   API 方向编码 → gimbal 库方向枚举
 */
static int dir_to_gimbal(int dir, gimbal_direction_t *out)
{
    switch (dir) {
    case 1: *out = GIMBAL_DIR_LEFT;  return 0;
    case 2: *out = GIMBAL_DIR_RIGHT; return 0;
    case 3: *out = GIMBAL_DIR_UP;    return 0;
    case 4: *out = GIMBAL_DIR_DOWN;  return 0;
    default: return -1;
    }
}

/**
 * @brief   非阻塞保存扫描限位预置位
 *
 *          只发送 set_pan + set_tilt + preset_set 三帧，中间各等 500ms，
 *          不轮询到位。扫描限位不需要精确匹配物理极限位置，
 *          差 0.1° 对扫描范围无实质影响。
 *
 * @param pan_degrees  限位水平角度
 * @param preset_id    保存到的预置位号（17=左限位，18=右限位）
 */
static int gimbal_save_scan_limit(gimbal_t *gimbal,
                                  double pan_degrees,
                                  uint8_t preset_id)
{
    printf("[PTZ] 非阻塞保存扫描限位: pan=%.1f° → preset %u\n",
           pan_degrees, preset_id);

    if (gimbal_set_pan(gimbal, pan_degrees) != 0) {
        fprintf(stderr, "[PTZ] scan limit set_pan 失败: %s\n", strerror(errno));
        return -1;
    }
    usleep(GIMBAL_POSITION_SETTLE_DELAY_US);

    /* tilt 保持 0° */
    if (gimbal_set_tilt(gimbal, 0.0) != 0) {
        fprintf(stderr, "[PTZ] scan limit set_tilt 失败: %s\n", strerror(errno));
        return -1;
    }
    usleep(GIMBAL_POSITION_SETTLE_DELAY_US);

    if (gimbal_preset_set(gimbal, preset_id) != 0) {
        fprintf(stderr, "[PTZ] scan limit preset_set 失败: %s\n", strerror(errno));
        return -1;
    }
    usleep(GIMBAL_SYNC_DELAY_US);

    printf("[PTZ] 扫描限位 preset %u 已保存\n", preset_id);
    return 0;
}

/**
 * @brief   退出自主运动模式（扫描/巡航）
 *
 *          扫描/巡航是硬件自主模式，标准 STOP 帧无效。
 *          先 drain RX 清空硬件上报帧、等待总线空闲，
 *          再用运动指令抢占退出自主模式后停止。
 *
 *          不检查 g_autonomous / g_moving 软件标志——
 *          程序重启后标志为 0 但硬件可能仍在扫描（状态持久化）。
 *
 *          调用者必须持有 g_gimbal_mutex。
 */
static void stop_autonomous_mode(void)
{
    printf("[PTZ] 停止当前运动 (autonomous=%d moving=%d)...\n",
           g_autonomous, g_moving);

    /*
     * 1. 清空 RX 缓冲：扫描期间硬件 ~29ms/帧 持续上报位置，
     *    必须等总线空闲再发指令，否则 RS-485 半双工碰撞导致双方失败。
     *    gimbal_drain_rx() 最多丢弃 140 字节（~20 帧），不会死循环。
     */
    gimbal_drain_rx(&g_gimbal);

    /* 2. 手动运动指令 → 退出自主模式 → 停止 */
    gimbal_move(&g_gimbal, GIMBAL_DIR_RIGHT, 0x01);
    usleep(50000);
    gimbal_stop(&g_gimbal);
    usleep(GIMBAL_SYNC_DELAY_US);

    g_moving     = 0;
    g_autonomous = 0;
    g_active_scan = 0;

    /*
     * 停止后查询硬件实际角度，更新缓存。
     * 此时硬件已退出自主模式，RS-485 总线空闲。
     */
    {
        double hw_pan = 0.0, hw_tilt = 0.0;
        if (gimbal_query_pan(&g_gimbal, &hw_pan) == 0) {
            g_current_h_angle = (int)(hw_pan + 0.5);
        }
        usleep(50000);
        if (gimbal_query_tilt(&g_gimbal, &hw_tilt) == 0) {
            g_current_v_angle = (int)(hw_tilt + 0.5);
        }
        printf("[PTZ] 停止后角度: h=%d° v=%d°\n",
               g_current_h_angle, g_current_v_angle);
    }

    printf("[PTZ] 运动已停止\n");
}

/* ======================================================================== */
/*  cmd=101  移动控制                                                          */
/* ======================================================================== */
static int handle_move(const cmd_t *cmd, cJSON **resp)
{
    int dir   = cmd_get_int(cmd, "dir", 0);
    int speed = cmd_get_int(cmd, "speed", SPEED_DEFAULT);
    int angle = cmd_get_int(cmd, "angle", 0);
    int stop  = cmd_get_int(cmd, "stop", 0);

    printf("[PTZ] 移动: dir=%d speed=%d angle=%d stop=%d\n",
           dir, speed, angle, stop);

    /* ---- stop=1: 无条件退出扫描序列 ---- */
    if (stop == 1) {
        printf("[PTZ] 停止: 无条件退出扫描序列\n");
        /*
         * 不依赖 g_autonomous / g_moving 内存标志（程序重启后为 0，
         * 但硬件端扫描状态是持久化的）。无条件发送完整退出序列。
         */
        hardware_force_stop();
        g_current_speed = 0;

        notify_status_changed();
        if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
        return 0;
    }

    /*
     * 非 stop 的新运动指令：先停止当前扫描/巡航/移动，
     * 否则硬件在扫描状态下会忽略后续的 move/goto 命令。
     */
    stop_autonomous_mode();

    /* ---- 参数校验 ---- */
    if (dir < 0 || dir > 4) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "dir 无效，应为 0(绝对定位)/1(左)/2(右)/3(上)/4(下)");
        return -1;
    }
    if (speed < SPEED_MIN) {
        printf("[PTZ] speed=%d 低于最小值，钳位到 %d\n", speed, SPEED_MIN);
        speed = SPEED_MIN;
    }
    if (speed > SPEED_MAX) {
        printf("[PTZ] speed=%d 超出最大值，钳位到 %d\n", speed, SPEED_MAX);
        speed = SPEED_MAX;
    }

    /* ======================================================================== */
    /*  dir=0  绝对定位：axis 选择水平/垂直（V3.2）                                 */
    /* ======================================================================== */
    if (dir == 0) {
        int axis = cmd_get_int(cmd, "axis", 1);

        if (axis < 1 || axis > 2) {
            if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                 "axis 无效，应为 1(水平) 或 2(垂直)");
            return -1;
        }

        double target_pan  = (double)g_current_h_angle;
        double target_tilt = (double)g_current_v_angle;

        if (axis == 1) {
            /* 水平绝对定位 */
            target_pan = (double)angle;
            if (target_pan  < 0.0)   target_pan  = 0.0;
            if (target_pan  > 350.0) target_pan  = 350.0;
        } else {
            /* 垂直绝对定位 */
            target_tilt = (double)angle;
            if (target_tilt < -32.0) target_tilt = -32.0;
            if (target_tilt > 23.0)  target_tilt = 23.0;
        }

        printf("[PTZ] 绝对定位(dir=0 axis=%d): 目标 pan=%.1f° tilt=%.1f°\n",
               axis, target_pan, target_tilt);

        if (gimbal_set_pan(&g_gimbal, target_pan) != 0) {
            if (resp) *resp = msg_build_response(ERR_DEVICE_COMM,
                                                 "水平角度设置失败");
            return -1;
        }
        usleep(GIMBAL_SYNC_DELAY_US);

        if (gimbal_set_tilt(&g_gimbal, target_tilt) != 0) {
            if (resp) *resp = msg_build_response(ERR_DEVICE_COMM,
                                                 "垂直角度设置失败");
            return -1;
        }

        g_moving = 1;
        g_autonomous = 0;
        g_current_speed = speed;
        g_current_h_angle = (int)(target_pan + 0.5);
        g_current_v_angle = (int)(target_tilt + 0.5);
        notify_status_changed();

        if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
        return 0;
    }

    /* ======================================================================== */
    /*  dir=1~4  有方向                                                          */
    /* ======================================================================== */
    gimbal_direction_t gimbal_dir;
    if (dir_to_gimbal(dir, &gimbal_dir) != 0) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "方向映射失败");
        return -1;
    }

    /* ---- angle > 0: 相对定位（V3.2） ---- */
    if (angle > 0) {
        /*
         * V3.2: angle 为相对当前位置的偏移量，非绝对角度。
         *
         * 先查询一次当前实际角度，按方向计算目标位置：
         *   dir=1(左):  target_pan  = current_pan  - angle
         *   dir=2(右):  target_pan  = current_pan  + angle
         *   dir=3(上):  target_tilt = current_tilt + angle
         *   dir=4(下):  target_tilt = current_tilt - angle
         *
         * 目标钳位到有效范围后直接发送，不轮询到位。
         */
        double cur_pan  = (double)g_current_h_angle;
        double cur_tilt = (double)g_current_v_angle;

        /*
         * 查询一次硬件实际角度（不轮询）。
         * 扫描/巡航期间 RS-485 被上报帧占满，查询会失败，回退到缓存值。
         */
        if (!g_autonomous) {
            double hw_pan = 0.0, hw_tilt = 0.0;
            if (gimbal_query_pan(&g_gimbal, &hw_pan) == 0) {
                cur_pan = hw_pan;
            } else {
                printf("[PTZ] 水平角度查询失败，使用缓存值 %.1f°\n", cur_pan);
            }
            usleep(GIMBAL_SYNC_DELAY_US);
            if (gimbal_query_tilt(&g_gimbal, &hw_tilt) == 0) {
                cur_tilt = hw_tilt;
            } else {
                printf("[PTZ] 垂直角度查询失败，使用缓存值 %.1f°\n", cur_tilt);
            }
        } else {
            printf("[PTZ] 自主模式中，跳过硬件查询，使用缓存值 (%.1f°, %.1f°)\n",
                   cur_pan, cur_tilt);
        }

        double target_pan  = cur_pan;
        double target_tilt = cur_tilt;

        switch (dir) {
        case 1: target_pan  = cur_pan  - (double)angle; break;  /* 左 */
        case 2: target_pan  = cur_pan  + (double)angle; break;  /* 右 */
        case 3: target_tilt = cur_tilt + (double)angle; break;  /* 上 */
        case 4: target_tilt = cur_tilt - (double)angle; break;  /* 下 */
        }

        printf("[PTZ] 相对定位: 当前(%.1f°,%.1f°) dir=%d offset=%d → 目标(%.1f°,%.1f°)\n",
               cur_pan, cur_tilt, dir, angle, target_pan, target_tilt);

        /* 钳位 */
        if (target_pan  < 0.0)   { printf("[PTZ] pan=%.1f 钳位到 0°\n", target_pan);   target_pan  = 0.0; }
        if (target_pan  > 350.0) { printf("[PTZ] pan=%.1f 钳位到 350°\n", target_pan); target_pan  = 350.0; }
        if (target_tilt < -32.0) { printf("[PTZ] tilt=%.1f 钳位到 -32°\n", target_tilt); target_tilt = -32.0; }
        if (target_tilt > 23.0)  { printf("[PTZ] tilt=%.1f 钳位到 23°\n", target_tilt);  target_tilt = 23.0; }

        if (gimbal_set_pan(&g_gimbal, target_pan) != 0) {
            if (resp) *resp = msg_build_response(ERR_DEVICE_COMM,
                                                 "水平角度设置失败");
            return -1;
        }
        usleep(GIMBAL_SYNC_DELAY_US);

        if (gimbal_set_tilt(&g_gimbal, target_tilt) != 0) {
            if (resp) *resp = msg_build_response(ERR_DEVICE_COMM,
                                                 "垂直角度设置失败");
            return -1;
        }

        g_moving = 1;
        g_autonomous = 0;
        g_current_speed = speed;
        g_current_h_angle = (int)(target_pan + 0.5);
        g_current_v_angle = (int)(target_tilt + 0.5);
        notify_status_changed();

        if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
        return 0;
    }

    /* ---- angle ≤ 0 或未指定: 持续转动 ---- */
    uint8_t speed_code = speed_to_code(speed);
    printf("[PTZ] 持续移动: dir=%d speed_code=0x%02X\n", dir, speed_code);

    if (gimbal_move(&g_gimbal, gimbal_dir, speed_code) != 0) {
        if (resp) *resp = msg_build_response(ERR_DEVICE_COMM, "移动指令发送失败");
        return -1;
    }

    g_moving = 1;
    g_autonomous = 0;
    g_current_speed = speed;
    notify_status_changed();

    if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=102  绝对角度归位（非阻塞）                                              */
/* ======================================================================== */
static int handle_goto(const cmd_t *cmd, cJSON **resp)
{
    int h_angle = cmd_get_int(cmd, "horizontalAngle", -999);
    int v_angle = cmd_get_int(cmd, "verticalAngle", -999);

    printf("[PTZ] 归位: h=%d v=%d\n", h_angle, v_angle);

    /* 先停止当前扫描/巡航/移动，确保硬件能响应定位命令 */
    stop_autonomous_mode();

    if (h_angle == -999) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "缺少 horizontalAngle");
        return -1;
    }
    if (v_angle == -999) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "缺少 verticalAngle");
        return -1;
    }

    if (h_angle < PTZ_H_ANGLE_MIN) {
        printf("[PTZ] horizontalAngle=%d 低于最小值，钳位到 %d\n", h_angle, PTZ_H_ANGLE_MIN);
        h_angle = PTZ_H_ANGLE_MIN;
    }
    if (h_angle > PTZ_H_ANGLE_MAX) {
        printf("[PTZ] horizontalAngle=%d 超出最大值，钳位到 %d\n", h_angle, PTZ_H_ANGLE_MAX);
        h_angle = PTZ_H_ANGLE_MAX;
    }
    if (v_angle < PTZ_V_ANGLE_MIN) {
        printf("[PTZ] verticalAngle=%d 低于最小值，钳位到 %d\n", v_angle, PTZ_V_ANGLE_MIN);
        v_angle = PTZ_V_ANGLE_MIN;
    }
    if (v_angle > PTZ_V_ANGLE_MAX) {
        printf("[PTZ] verticalAngle=%d 超出最大值，钳位到 %d\n", v_angle, PTZ_V_ANGLE_MAX);
        v_angle = PTZ_V_ANGLE_MAX;
    }

    printf("[PTZ] 设置角度: pan=%d tilt=%d\n", h_angle, v_angle);

    if (gimbal_set_pan(&g_gimbal, (double)h_angle) != 0) {
        if (resp) *resp = msg_build_response(ERR_DEVICE_COMM,
                                             "水平角度设置失败");
        return -1;
    }
    usleep(GIMBAL_SYNC_DELAY_US);

    if (gimbal_set_tilt(&g_gimbal, (double)v_angle) != 0) {
        if (resp) *resp = msg_build_response(ERR_DEVICE_COMM,
                                             "垂直角度设置失败");
        return -1;
    }

    g_current_h_angle = h_angle;
    g_current_v_angle = v_angle;
    g_moving = 1;
    g_autonomous = 0;

    printf("[PTZ] 归位指令已发送: h=%d v=%d\n",
           g_current_h_angle, g_current_v_angle);

    notify_status_changed();
    if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=103  预置位设置（逐点轮询到位 + 每点成功/失败跟踪）                       */
/* ======================================================================== */
/**
 * @brief   构建 cmd=103 预置位设置响应 JSON
 *
 *          格式:
 *          {
 *            "code": 0, "cmd": 103, "msg": "success",
 *            "groupNum": <int>,
 *            "total": <int>, "successCount": <int>, "failCount": <int>,
 *            "groupData": [ {"index":N, "horAngle":H, "verAngle":V, "success":B}, ... ]
 *          }
 */
static cJSON *build_preset_set_response(int code, const char *msg,
                                         int group_num,
                                         ptz_preset_group_t *group,
                                         int *success_flags,
                                         int total, int success_count,
                                         int fail_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code",         code);
    cJSON_AddNumberToObject(root, "cmd",          103);
    cJSON_AddStringToObject(root, "msg",          msg ? msg : "");
    cJSON_AddNumberToObject(root, "groupNum",     group_num);
    cJSON_AddNumberToObject(root, "total",        total);
    cJSON_AddNumberToObject(root, "successCount", success_count);
    cJSON_AddNumberToObject(root, "failCount",    fail_count);

    cJSON *arr = cJSON_AddArrayToObject(root, "groupData");
    for (int i = 0; i < group->count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index",    group->points[i].index);
        cJSON_AddNumberToObject(item, "horAngle", group->points[i].hor_angle);
        cJSON_AddNumberToObject(item, "verAngle", group->points[i].ver_angle);
        cJSON_AddBoolToObject  (item, "success",  success_flags
                                ? (success_flags[i] != 0) : false);
        cJSON_AddItemToArray(arr, item);
    }

    return root;
}

static int handle_preset(const cmd_t *cmd, cJSON **resp)
{
    int group_num = cmd_get_int(cmd, "groupNum", 0);

    printf("[PTZ] 预置位设置: groupNum=%d\n", group_num);

    /* 先停止当前扫描/巡航/移动，确保硬件能响应预置位命令 */
    stop_autonomous_mode();

    /* 清除中断标志（可能在上一轮预置位设置中被设置） */
    g_preset_abort = 0;

    if (group_num < 1 || group_num > PTZ_PRESET_GROUP_COUNT) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "groupNum 无效，应为 1 或 2");
        return -1;
    }

    cJSON *group_data = cJSON_GetObjectItem(cmd->root, "groupData");
    if (!group_data || !cJSON_IsArray(group_data)) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "缺少 groupData 或非数组");
        return -1;
    }

    int n = cJSON_GetArraySize(group_data);
    if (n <= 0 || n > PTZ_PRESET_MAX_POINTS) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "groupData 数量无效 (1~16)");
        return -1;
    }

    ptz_preset_group_t *group = &g_preset_groups[group_num - 1];
    group->count = 0;

    /* 第一遍：参数校验 + 缓存 */
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(group_data, i);
        if (!item) continue;

        cJSON *j_index = cJSON_GetObjectItem(item, "index");
        cJSON *j_hor   = cJSON_GetObjectItem(item, "horAngle");
        cJSON *j_ver   = cJSON_GetObjectItem(item, "verAngle");

        if (!cJSON_IsNumber(j_index) || !cJSON_IsNumber(j_hor) ||
            !cJSON_IsNumber(j_ver)) {
            if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                 "groupData 元素字段不完整");
            return -1;
        }

        int idx = j_index->valueint;
        int hor = j_hor->valueint;
        int ver = j_ver->valueint;

        if (idx < 0 || idx >= PTZ_PRESET_MAX_POINTS) {
            if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                                 "index 超出范围 0~15");
            return -1;
        }
        if (hor < PTZ_H_ANGLE_MIN) {
            printf("[PTZ] horAngle=%d 低于最小值，钳位到 %d\n", hor, PTZ_H_ANGLE_MIN);
            hor = PTZ_H_ANGLE_MIN;
        }
        if (hor > PTZ_H_ANGLE_MAX) {
            printf("[PTZ] horAngle=%d 超出最大值，钳位到 %d\n", hor, PTZ_H_ANGLE_MAX);
            hor = PTZ_H_ANGLE_MAX;
        }
        if (ver < PTZ_V_ANGLE_MIN) {
            printf("[PTZ] verAngle=%d 低于最小值，钳位到 %d\n", ver, PTZ_V_ANGLE_MIN);
            ver = PTZ_V_ANGLE_MIN;
        }
        if (ver > PTZ_V_ANGLE_MAX) {
            printf("[PTZ] verAngle=%d 超出最大值，钳位到 %d\n", ver, PTZ_V_ANGLE_MAX);
            ver = PTZ_V_ANGLE_MAX;
        }

        group->points[i].index     = idx;
        group->points[i].hor_angle = hor;
        group->points[i].ver_angle = ver;
        group->points[i].success   = 0;   /* 待写入确认 */
    }
    group->count = n;

    /* 第二遍：逐点移动到位 + 保存预置位 + 重试失败项（最多 3 轮） */
    int preset_base    = (group_num == 2) ? 33 : 1;
    int success_flags[PTZ_PRESET_MAX_POINTS] = {0};
    int total          = n;

    g_moving      = 1;
    g_autonomous  = 0;

    /*
     * 先清除全部旧预置位，再写入新预置位。
     *
     * Pelco-D 巡航路线（route 1 → presets 1~16, route 2 → presets 33~48）
     * 是硬件固化的全遍历，遇到未保存的空预置位会停止巡航。
     * 如果用户只配置了部分预置位（如 3 个），不清除旧预置位会导致巡航
     * 在第一个空预置位处卡住。
     *
     * 注意：gimbal_clear_presets() 清除的是全部预置位（含另一组），
     * 但配置文件和内存中的预置位数据不会丢失——本轮写入会把当前组的
     * 预置位重新保存到硬件。如果另一组也需要巡航，需重新执行 cmd=103。
     */
    printf("[PTZ] 清除全部旧预置位...\n");
    if (gimbal_clear_presets(&g_gimbal) != 0) {
        printf("[PTZ] 警告: 清除旧预置位失败，继续设置\n");
    }
    usleep(GIMBAL_SYNC_DELAY_US);

    /* 同步清除 YAML 持久化数据，硬件和文件保持一致 */
    config_manager_clear_presets();

    /*
     * 清空另一组的内存缓存。
     * gimbal_clear_presets() 已清除硬件端全部预置位（含另一组），
     * 内存缓存必须同步清空，否则后续 YAML 保存时会写入过期数据。
     */
    if (group_num == 1) {
        g_preset_groups[1].count = 0;
    } else {
        g_preset_groups[0].count = 0;
    }

    /*
     * 单次预置位设置：
     *   1. 移动到目标角度（内联轮询到位，0.1° 容差，可被 abort 中断）
     *   2. gimbal_preset_set()       — 保存当前位为 Pelco-D 预置位
     */
    for (int round = 0; round < 3; round++) {
        int round_success = 0;

        for (int i = 0; i < n; i++) {
            /* 前几轮已成功的跳过 */
            if (success_flags[i]) continue;

            int     preset_id   = preset_base + group->points[i].index;
            double  target_pan  = (double)group->points[i].hor_angle;
            double  target_tilt = (double)group->points[i].ver_angle;

            printf("[PTZ]   预置位[%d]: 第%d轮 id=%d pan=%.1f tilt=%.1f ... ",
                   i, round + 1, preset_id, target_pan, target_tilt);

            /*
             * 内联移动到位 + 位置验证（等效 gimbal_move_to_position()，
             * 但加入 abort 检查点）：
             *   1. set_pan → usleep(500ms) → set_tilt
             *   2. 每 500ms 查询 gimbal_get_status()，容差 0.1°
             *   3. 每次轮询检查 g_preset_abort，可被主线程 stop 指令中断
             *   4. 超时 120s
             *
             * 调用者已持有 g_gimbal_mutex，保证串口访问互斥。
             */
            int ret = -1;
            if (gimbal_set_pan(&g_gimbal, target_pan) == 0) {
                usleep(GIMBAL_POSITION_SETTLE_DELAY_US);
                if (gimbal_set_tilt(&g_gimbal, target_tilt) == 0) {
                    const int max_attempts =
                        PTZ_POSITION_TIMEOUT_MS /
                        (GIMBAL_POSITION_SETTLE_DELAY_US / 1000);
                    for (int attempt = 0; attempt < max_attempts; attempt++) {
                        usleep(GIMBAL_POSITION_SETTLE_DELAY_US);

                        /* 检查中断标志：主线程收到 stop 时设置 */
                        if (g_preset_abort) {
                            printf("中断(收到停止指令)\n");
                            hardware_force_stop();
                            goto preset_abort;
                        }

                        gimbal_status_t status;
                        if (gimbal_get_status(&g_gimbal, &status) == 0 &&
                            status.pan_valid && status.tilt_valid &&
                            fabs(status.pan_degrees - target_pan)
                                <= PTZ_POSITION_TOLERANCE_DEG &&
                            fabs(status.tilt_degrees - target_tilt)
                                <= PTZ_POSITION_TOLERANCE_DEG) {
                            ret = 0;
                            break;
                        }
                    }
                }
            }

            if (ret == 0) {
                /* 移动到位后保存预置位 */
                ret = gimbal_preset_set(&g_gimbal, (uint8_t)preset_id);
                if (ret == 0) {
                    /* 更新角度缓存 */
                    g_current_h_angle = (int)(target_pan + 0.5);
                    g_current_v_angle = (int)(target_tilt + 0.5);
                }
            }

            if (ret == 0) {
                success_flags[i] = 1;
                group->points[i].success = 1;
                round_success++;
                printf("成功\n");
            } else {
                group->points[i].success = 0;
                printf("失败\n");
            }

            /* 预置位间等待 100ms，让硬件消化上一轮的写入 */
            usleep(GIMBAL_SYNC_DELAY_US);
        }

        if (round == 0) {
            printf("[PTZ] 第1轮完成: %d/%d 成功\n", round_success, n);
        } else {
            printf("[PTZ] 第%d轮重试完成: 修正 %d 个\n", round + 1, round_success);
        }

        /* 全部成功则提前结束 */
        int remaining = 0;
        for (int i = 0; i < n; i++) {
            if (!success_flags[i]) remaining++;
        }
        if (remaining == 0) break;
    }

    /*
     * 正常结束或 abort 中断都会跳转到这里。
     * 统计已成功的预置位，未完成的标记为失败。
     */
    int success_count = 0;
    int fail_count    = 0;

preset_abort:
    g_moving = 0;
    success_count = 0;
    for (int i = 0; i < n; i++) {
        if (success_flags[i]) success_count++;
    }
    fail_count = total - success_count;

    printf("[PTZ] 预置位设置完成: 组%d, 总计%d 成功%d 失败%d\n",
           group_num, total, success_count, fail_count);

    notify_status_changed();

    /* 构建带逐点成功/失败信息和汇总字段的响应 */
    if (resp) {
        const char *msg_text;
        int         code;
        if (g_preset_abort) {
            code     = ERR_DEVICE_COMM;
            msg_text = "预置位设置被中断";
            g_preset_abort = 0;  /* 清除标志，允许后续预置位操作 */
        } else if (fail_count > 0) {
            code     = ERR_DEVICE_COMM;
            msg_text = "部分预置位设置失败";
        } else {
            code     = ERR_SUCCESS;
            msg_text = "success";
        }
        *resp = build_preset_set_response(
            code, msg_text,
            group_num, group, success_flags,
            total, success_count, fail_count);
    }

    /*
     * 返回 0 表示至少有一个预置位设置成功，
     * 全部失败返回 -1 触发 g_fail_status 更新。
     */
    return (success_count > 0) ? 0 : -1;
}

/* ======================================================================== */
/*  cmd=104  巡航                                                              */
/* ======================================================================== */
static int handle_cruise(const cmd_t *cmd, cJSON **resp)
{
    int group_num = cmd_get_int(cmd, "groupNum", 0);
    int dwell     = cmd_get_int(cmd, "dwell", 0);      /* 0=5s, 1=10s, 2=15s */
    int speed     = cmd_get_int(cmd, "speed", SPEED_DEFAULT);

    printf("[PTZ] 巡航: groupNum=%d dwell=%d speed=%d\n", group_num, dwell, speed);

    /* 先停止当前扫描/巡航/移动，确保硬件能响应巡航命令 */
    stop_autonomous_mode();

    if (group_num < 1 || group_num > PTZ_PRESET_GROUP_COUNT) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "groupNum 无效，应为 1 或 2");
        return -1;
    }
    if (dwell < 0 || dwell > 2) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "dwell 无效，应为 0(5s)/1(10s)/2(15s)");
        return -1;
    }
    if (speed < SPEED_MIN) {
        printf("[PTZ] speed=%d 低于最小值，钳位到 %d\n", speed, SPEED_MIN);
        speed = SPEED_MIN;
    }
    if (speed > SPEED_MAX) {
        printf("[PTZ] speed=%d 超出最大值，钳位到 %d\n", speed, SPEED_MAX);
        speed = SPEED_MAX;
    }

    ptz_preset_group_t *group = &g_preset_groups[group_num - 1];
    if (group->count == 0) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "该组尚未设置预置位，请先执行 cmd=103");
        return -1;
    }

    /* ---- 设置巡航停留时间（独立 dwell 参数，不再从 speed 推导） ---- */
    printf("[PTZ] gimbal_cruise_set_dwell: mode=%u (%s)\n",
           (unsigned int)dwell,
           dwell == 0 ? "5秒" : dwell == 1 ? "10秒" : "15秒");
    if (gimbal_cruise_set_dwell(&g_gimbal, (uint8_t)dwell) != 0) {
        if (resp) *resp = msg_build_response(ERR_DEVICE_COMM, "巡航停留时间设置失败");
        return -1;
    }

    /* 等待硬件处理巡航停留时间设置 */
    usleep(GIMBAL_SYNC_DELAY_US);

    /* ---- 启动巡航 ---- */
    printf("[PTZ] gimbal_cruise_start: route=%d\n", group_num);
    if (gimbal_cruise_start(&g_gimbal, (uint8_t)group_num) != 0) {
        if (resp) *resp = msg_build_response(ERR_DEVICE_COMM, "巡航启动失败");
        return -1;
    }

    g_moving = 1;
    g_autonomous = 1;
    printf("[PTZ] 巡航已启动: 路线%d\n", group_num);

    notify_status_changed();
    if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=105  扫描                                                              */
/*                                                                            */
/*  [修复] 用 Preset Call 切换退出上一次扫描，而非通用停止帧。                      */
/*  增加 g_active_scan 跟踪当前扫描类型，确保停止和重启正确。                       */
/* ======================================================================== */
static int handle_scan(const cmd_t *cmd, cJSON **resp)
{
    int scan_dir = cmd_get_int(cmd, "scanDir", 0);
    int speed    = cmd_get_int(cmd, "speed", SPEED_DEFAULT);

    printf("[PTZ] 扫描: scanDir=%d speed=%d\n", scan_dir, speed);

    if (scan_dir != 1) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID,
                                             "scanDir 无效，仅支持 1(水平扫描)");
        return -1;
    }
    if (speed < SPEED_MIN) {
        printf("[PTZ] speed=%d 低于最小值，钳位到 %d\n", speed, SPEED_MIN);
        speed = SPEED_MIN;
    }
    if (speed > SPEED_MAX) {
        printf("[PTZ] speed=%d 超出最大值，钳位到 %d\n", speed, SPEED_MAX);
        speed = SPEED_MAX;
    }

    /* ---- 如果正在自主运动（扫描/巡航），先正确退出 ---- */
    stop_autonomous_mode();

    /* ---- 设置线扫速度 ---- */
    uint8_t scan_speed = speed_to_scan_code(speed);
    printf("[PTZ] gimbal_scan_set_speed: code=%u\n", scan_speed);
    if (gimbal_scan_set_speed(&g_gimbal, scan_speed) != 0) {
        if (resp) *resp = msg_build_response(ERR_DEVICE_COMM, "线扫速度设置失败");
        return -1;
    }

    /* 等待硬件处理完扫描速度设置 */
    usleep(GIMBAL_SYNC_DELAY_US);

    /* ---- 启动最大角度扫描（preset call 89） ---- */
    printf("[PTZ] gimbal_scan_start_max (最大角度扫描)\n");
    if (gimbal_scan_start_max(&g_gimbal) != 0) {
        if (resp) *resp = msg_build_response(ERR_DEVICE_COMM, "扫描启动失败");
        return -1;
    }

    g_moving      = 1;
    g_autonomous  = 1;
    g_active_scan = 1;
    printf("[PTZ] 扫描已启动 (max, horizontal)\n");

    notify_status_changed();
    if (resp) *resp = msg_build_response(ERR_SUCCESS, "success");
    return 0;
}

/* ======================================================================== */
/*  cmd=106  预置位查询                                                        */
/* ======================================================================== */
/**
 * @brief   构建 cmd=106 预置位查询响应 JSON
 *
 *          格式:
 *          {
 *            "code": 0, "cmd": 106, "msg": "success",
 *            "groupNum": <int>,
 *            "groupData": [ {"index":N, "horAngle":H, "verAngle":V}, ... ]
 *          }
 */
static cJSON *build_preset_query_response(int code, const char *msg,
                                           int group_num,
                                           ptz_preset_group_t *group)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "cmd",  106);
    cJSON_AddStringToObject(root, "msg",  msg ? msg : "");
    cJSON_AddNumberToObject(root, "groupNum", group_num);

    cJSON *arr = cJSON_AddArrayToObject(root, "groupData");
    if (group) {
        for (int i = 0; i < group->count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index",    group->points[i].index);
            cJSON_AddNumberToObject(item, "horAngle", group->points[i].hor_angle);
            cJSON_AddNumberToObject(item, "verAngle", group->points[i].ver_angle);
            cJSON_AddItemToArray(arr, item);
        }
    }

    return root;
}

static int handle_preset_query(const cmd_t *cmd, cJSON **resp)
{
    int group_num = cmd_get_int(cmd, "groupNum", 0);

    printf("[PTZ] 预置位查询: groupNum=%d\n", group_num);

    if (group_num < 1 || group_num > PTZ_PRESET_GROUP_COUNT) {
        if (resp) *resp = build_preset_query_response(
            ERR_PARAM_INVALID, "groupNum 无效，应为 1 或 2", group_num, NULL);
        return -1;
    }

    ptz_preset_group_t *group = &g_preset_groups[group_num - 1];

    if (group->count == 0) {
        printf("[PTZ] 预置位查询: 组%d 无数据\n", group_num);
    } else {
        printf("[PTZ] 预置位查询: 组%d 共 %d 个预置位\n", group_num, group->count);
        for (int i = 0; i < group->count; i++) {
            printf("[PTZ]   [%d] index=%d hor=%d ver=%d\n",
                   i, group->points[i].index,
                   group->points[i].hor_angle,
                   group->points[i].ver_angle);
        }
    }

    if (resp) {
        *resp = build_preset_query_response(ERR_SUCCESS, "success",
                                             group_num, group);
    }
    return 0;
}

/* ======================================================================== */
/*  公开接口实现                                                               */
/* ======================================================================== */

int device_ptz_init(const char *uart_dev, int baudrate)
{
    gimbal_config_t config;
    gimbal_config_init_default(&config);

    if (uart_dev) {
        config.device = uart_dev;
    }
    if (baudrate > 0) {
        config.baudrate = baudrate;
    }

    printf("[PTZ] 初始化: device=%s baudrate=%d address=0x%02X\n",
           config.device, config.baudrate, config.address);

    if (gimbal_init(&g_gimbal, &config) != 0) {
        fprintf(stderr, "[PTZ] gimbal_init 失败: %s\n", strerror(errno));
        g_fail_status = 1;  /* 不在线 */
        return -1;
    }

    g_current_h_angle = 0;
    g_current_v_angle = 0;
    g_moving          = 0;
    g_active          = 1;
    g_autonomous      = 0;
    g_status_changed  = 0;
    g_status_changed_cb = NULL;
    g_left_limit_set  = 0;
    g_right_limit_set = 0;
    g_active_scan     = 0;
    memset(g_preset_groups, 0, sizeof(g_preset_groups));

    /*
     * 上电后通过运动指令 + 停止来退出硬件自主扫描模式。
     *
     * 云台上电自检（preset 105）或全角度扫描（preset 89）是硬件自主模式，
     * Pelco-D 标准 STOP 帧 (FF 01 00 00 00 00 01) 只能停止手动转动，
     * 对自主模式无效。需要先发一条手动控制指令 (gimbal_move) 抢占总线，
     * 将硬件从自主模式拉回手动模式，再发 STOP 停止。
     *
     * 策略：发一条极低速右转指令 → 等 50ms 让硬件退出自主模式 → STOP。
     */
    printf("[PTZ] 退出硬件自主扫描模式...\n");
    gimbal_drain_rx(&g_gimbal);  /* 清空硬件上报帧，避免 RS-485 碰撞 */
    gimbal_move(&g_gimbal, GIMBAL_DIR_RIGHT, 0x01);
    usleep(50000);
    gimbal_stop(&g_gimbal);
    printf("[PTZ] 停止帧已发送\n");

    /*
     * 停止后查询一次硬件实际角度，初始化位置缓存。
     * 此时硬件已退出自主模式，查询可以正常收到响应。
     * 查询失败不阻塞初始化，等服务端下发 cmd=102 定位指令后校正。
     */
    {
        double init_pan = 0.0, init_tilt = 0.0;
        if (gimbal_query_pan(&g_gimbal, &init_pan) == 0) {
            g_current_h_angle = (int)(init_pan + 0.5);
            printf("[PTZ] 初始水平角度: %d° (硬件查询)\n", g_current_h_angle);
        } else {
            printf("[PTZ] 初始水平角度查询失败，保持默认值 0°\n");
        }
        usleep(GIMBAL_SYNC_DELAY_US);
        if (gimbal_query_tilt(&g_gimbal, &init_tilt) == 0) {
            g_current_v_angle = (int)(init_tilt + 0.5);
            printf("[PTZ] 初始垂直角度: %d° (硬件查询)\n", g_current_v_angle);
        } else {
            printf("[PTZ] 初始垂直角度查询失败，保持默认值 0°\n");
        }
    }

    printf("[PTZ] 初始化完成\n");
    return 0;
}

void device_ptz_set_status_cb(ptz_status_cb_t cb)
{
    g_status_changed_cb = cb;
}

int device_ptz_poll_status_changed(void)
{
    if (g_status_changed) {
        g_status_changed = 0;
        return 1;
    }
    return 0;
}

int device_ptz_execute(const cmd_t *cmd, cJSON **resp)
{
    if (!cmd) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "cmd 为空");
        return -1;
    }

    printf("[PTZ] cmd=%d\n", cmd->cmd_id);

    if (cmd->cmd_id < 101 || cmd->cmd_id > 106) {
        if (resp) *resp = msg_build_response(ERR_PARAM_INVALID, "无效的 cmd");
        return -1;
    }

    /*
     * 加锁保护：心跳线程可能同时在调 device_ptz_get_status() 读取
     * g_current_h_angle / g_current_v_angle 等共享变量。
     * handle_* 函数会写串口和这些变量，需互斥。
     */
    pthread_mutex_lock(&g_gimbal_mutex);

    int ret = -1;
    switch (cmd->cmd_id) {
    case 101: ret = handle_move(cmd, resp);        break;
    case 102: ret = handle_goto(cmd, resp);        break;
    case 103: ret = handle_preset(cmd, resp);      break;
    case 104: ret = handle_cruise(cmd, resp);      break;
    case 105: ret = handle_scan(cmd, resp);        break;
    case 106: ret = handle_preset_query(cmd, resp); break;
    }

    if (ret == 0) {
        g_fail_status = 0;  /* 命令执行成功，恢复正常 */
        notify_status_changed();
    } else {
        g_fail_status = 2;  /* 响应异常 */
    }

    pthread_mutex_unlock(&g_gimbal_mutex);
    return ret;
}

cJSON *device_ptz_get_status(void)
{
    /*
     * 使用 trylock 防阻塞。本函数被心跳线程和 config_manager 调用，
     * PTZ worker 执行长耗时操作时持有 g_gimbal_mutex，trylock 失败则
     * 无锁读取 int 缓存值（ARM/x86 上 int 读取为单指令，安全）。
     */
    int h_angle, v_angle, speed, moving, autonomous, active_scan, fail;

    if (pthread_mutex_trylock(&g_gimbal_mutex) == 0) {
        h_angle     = g_current_h_angle;
        v_angle     = g_current_v_angle;
        speed       = g_current_speed;
        moving      = g_moving;
        autonomous  = g_autonomous;
        active_scan = g_active_scan;
        fail        = g_fail_status;
        pthread_mutex_unlock(&g_gimbal_mutex);
    } else {
        h_angle     = g_current_h_angle;
        v_angle     = g_current_v_angle;
        speed       = g_current_speed;
        moving      = g_moving;
        autonomous  = g_autonomous;
        active_scan = g_active_scan;
        fail        = g_fail_status;
    }

    cJSON *status = cJSON_CreateObject();
    cJSON_AddBoolToObject(status, "online", (fail != 1));

    /*
     * 根据运行模式设置 state 字段：
     *   autonomous && active_scan  → "scanning"  (cmd=105)
     *   autonomous && !active_scan → "cruising"  (cmd=104)
     *   !autonomous && moving      → "moving"    (cmd=101/102 手动运动)
     *   !autonomous && !moving     → "idle"
     *
     * 自主模式（扫描/巡航）期间 RS-485 总线被硬件上报帧占满（~29ms/帧），
     * 禁止查询硬件角度，响应中不返回 horizontalAngle/verticalAngle，
     * 仅返回运行模式和速度。
     */
    const char *state_str;
    if (autonomous && active_scan) {
        state_str = "scanning";
    } else if (autonomous && !active_scan) {
        state_str = "cruising";
    } else if (!autonomous && moving) {
        state_str = "moving";
    } else {
        state_str = "idle";
    }
    cJSON_AddStringToObject(status, "state", state_str);

    cJSON_AddNumberToObject(status, "speed", speed);

    /* 仅在非自主模式下返回角度（自主模式角度持续变化，返回无意义） */
    if (!autonomous) {
        cJSON_AddNumberToObject(status, "horizontalAngle", h_angle);
        cJSON_AddNumberToObject(status, "verticalAngle",   v_angle);
    }

    return status;
}

/**
 * @brief   主动查询硬件当前角度并更新缓存（服务端主动查询用）
 *
 *          发送 Pelco-D 查询指令读取云台实际水平/俯仰角度，
 *          更新 g_current_h_angle / g_current_v_angle 缓存。
 *
 *          注意：扫描/巡航期间 RS-485 总线被硬件上报帧占满，
 *          查询会失败。此时保持缓存值不变，调用者应检查返回值。
 *
 * @return  0 成功，-1 失败（设备不在线或通信超时）
 */
int device_ptz_query_position(void)
{
    if (!g_active) {
        return -1;
    }

    /*
     * 扫描/巡航期间 RS-485 总线被硬件上报帧占满（~29ms/帧），
     * 查询必定超时。此时 gimbal_send_query() 内部 drain_rx 丢帧
     * + 5 轮超时等待约 2~3 秒，若调用者在主线程会冻结整个主循环。
     * 直接返回 -1，用缓存值。
     */
    if (g_autonomous) {
        printf("[PTZ] 自主模式中，跳过硬件角度查询（使用缓存值）\n");
        return -1;
    }

    pthread_mutex_lock(&g_gimbal_mutex);

    double pan = 0.0, tilt = 0.0;

    /* 先清空 RX 缓冲，避免读到扫描期间硬件上报的残留帧 */
    gimbal_drain_rx(&g_gimbal);

    if (gimbal_query_pan(&g_gimbal, &pan) != 0) {
        fprintf(stderr, "[PTZ] 查询水平角度失败\n");
        pthread_mutex_unlock(&g_gimbal_mutex);
        return -1;
    }
    usleep(GIMBAL_SYNC_DELAY_US);

    if (gimbal_query_tilt(&g_gimbal, &tilt) != 0) {
        fprintf(stderr, "[PTZ] 查询垂直角度失败\n");
        pthread_mutex_unlock(&g_gimbal_mutex);
        return -1;
    }

    g_current_h_angle = (int)(pan + 0.5);
    g_current_v_angle = (int)(tilt + 0.5);

    printf("[PTZ] 硬件角度查询成功: h=%d° v=%d°\n",
           g_current_h_angle, g_current_v_angle);

    pthread_mutex_unlock(&g_gimbal_mutex);
    return 0;
}

int device_ptz_get_fail_status(void)
{
    return g_fail_status;
}

int device_ptz_get_preset_group(int group_num, ptz_preset_group_t *group)
{
    if (!group) return -1;
    if (group_num < 1 || group_num > PTZ_PRESET_GROUP_COUNT) return -1;

    pthread_mutex_lock(&g_gimbal_mutex);
    memcpy(group, &g_preset_groups[group_num - 1], sizeof(ptz_preset_group_t));
    pthread_mutex_unlock(&g_gimbal_mutex);

    return (group->count > 0) ? 0 : -1;
}

int device_ptz_load_preset_group(int group_num, cJSON *json)
{
    if (!json) return -1;
    if (group_num < 1 || group_num > PTZ_PRESET_GROUP_COUNT) return -1;

    cJSON *arr = cJSON_GetObjectItem(json, "groupData");
    if (!cJSON_IsArray(arr)) return -1;

    int n = cJSON_GetArraySize(arr);
    if (n <= 0 || n > PTZ_PRESET_MAX_POINTS) return -1;

    pthread_mutex_lock(&g_gimbal_mutex);

    ptz_preset_group_t *group = &g_preset_groups[group_num - 1];
    group->count = 0;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;

        cJSON *j_idx = cJSON_GetObjectItem(item, "index");
        cJSON *j_hor = cJSON_GetObjectItem(item, "horAngle");
        cJSON *j_ver = cJSON_GetObjectItem(item, "verAngle");
        cJSON *j_suc = cJSON_GetObjectItem(item, "success");

        if (!cJSON_IsNumber(j_idx) || !cJSON_IsNumber(j_hor) ||
            !cJSON_IsNumber(j_ver)) {
            continue;
        }

        int idx = j_idx->valueint;
        int hor = j_hor->valueint;
        int ver = j_ver->valueint;
        int suc = cJSON_IsBool(j_suc) ? (cJSON_IsTrue(j_suc) ? 1 : 0)
                 : cJSON_IsNumber(j_suc) ? j_suc->valueint
                 : 1;   /* 旧格式无此字段时默认成功 */

        if (idx < 0 || idx >= PTZ_PRESET_MAX_POINTS) continue;
        if (hor < PTZ_H_ANGLE_MIN || hor > PTZ_H_ANGLE_MAX) continue;
        if (ver < PTZ_V_ANGLE_MIN || ver > PTZ_V_ANGLE_MAX) continue;

        group->points[i].index     = idx;
        group->points[i].hor_angle = hor;
        group->points[i].ver_angle = ver;
        group->points[i].success   = suc;
    }
    group->count = n;

    pthread_mutex_unlock(&g_gimbal_mutex);

    printf("[PTZ] 预置位组%d 已从配置文件加载: %d 个点\n", group_num, n);
    return 0;
}

cJSON *device_ptz_export_preset_group(int group_num)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "groupNum", group_num);

    cJSON *arr = cJSON_AddArrayToObject(root, "groupData");

    if (group_num >= 1 && group_num <= PTZ_PRESET_GROUP_COUNT) {
        pthread_mutex_lock(&g_gimbal_mutex);
        ptz_preset_group_t *group = &g_preset_groups[group_num - 1];
        for (int i = 0; i < group->count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index",    group->points[i].index);
            cJSON_AddNumberToObject(item, "horAngle", group->points[i].hor_angle);
            cJSON_AddNumberToObject(item, "verAngle", group->points[i].ver_angle);
            cJSON_AddBoolToObject  (item, "success",  group->points[i].success != 0);
            cJSON_AddItemToArray(arr, item);
        }
        pthread_mutex_unlock(&g_gimbal_mutex);
    }

    return root;
}

void device_ptz_abort_preset(void)
{
    g_preset_abort = 1;
    printf("[PTZ] 预置位中断请求已设置\n");
}

/**
 * @brief   手动设置扫描限位（非阻塞）
 *
 *          发送 set_pan + set_tilt 命令后等待固定 500ms 即保存预置位，
 *          不轮询到位。适合扫描限位这种不需要精确匹配的场景。
 *
 * @param left_pan   左限位水平角度（度）
 * @param right_pan  右限位水平角度（度）
 */
int device_ptz_set_scan_limits(double left_pan, double right_pan)
{
    if (!g_active) {
        fprintf(stderr, "[PTZ] 模块未初始化\n");
        errno = ENOTCONN;
        return -1;
    }

    printf("[PTZ] 手动设置扫描限位: 左=%.1f° 右=%.1f°\n", left_pan, right_pan);

    pthread_mutex_lock(&g_gimbal_mutex);

    if (gimbal_save_scan_limit(&g_gimbal, left_pan, 17) < 0) {
        fprintf(stderr, "[PTZ] 设置左扫描限位失败\n");
        pthread_mutex_unlock(&g_gimbal_mutex);
        return -1;
    }

    if (gimbal_save_scan_limit(&g_gimbal, right_pan, 18) < 0) {
        fprintf(stderr, "[PTZ] 设置右扫描限位失败\n");
        pthread_mutex_unlock(&g_gimbal_mutex);
        return -1;
    }

    pthread_mutex_unlock(&g_gimbal_mutex);

    g_left_limit_set  = 1;
    g_right_limit_set = 1;

    printf("[PTZ] 扫描限位设置完成: 左=%.1f° 右=%.1f°\n", left_pan, right_pan);
    return 0;
}

void device_ptz_deinit(void)
{
    pthread_mutex_lock(&g_gimbal_mutex);

    if (g_active) {
        gimbal_stop(&g_gimbal);
        gimbal_deinit(&g_gimbal);
    }

    g_active            = 0;
    g_autonomous        = 0;
    g_current_h_angle   = 0;
    g_current_v_angle   = 0;
    g_moving            = 0;
    g_status_changed    = 0;
    g_status_changed_cb = NULL;
    g_left_limit_set    = 0;
    g_right_limit_set   = 0;
    g_active_scan       = 0;
    memset(g_preset_groups, 0, sizeof(g_preset_groups));

    pthread_mutex_unlock(&g_gimbal_mutex);

    printf("[PTZ] 云台模块已释放\n");
}