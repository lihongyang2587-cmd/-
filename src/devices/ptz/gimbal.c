#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "gimbal.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pelco_d.h"

#define GIMBAL_SYNC_DELAY_US 100000
#define GIMBAL_QUERY_TIMEOUT_MS 1000
#define GIMBAL_PAN_MIN_DEGREES 0.0
#define GIMBAL_PAN_MAX_DEGREES 350.0
#define GIMBAL_TILT_MIN_DEGREES -32.0
#define GIMBAL_TILT_MAX_DEGREES 23.0
#define GIMBAL_POSITION_TOLERANCE_DEG 0.1
#define GIMBAL_POSITION_SETTLE_DELAY_US 500000
#define GIMBAL_POSITION_TIMEOUT_MS 120000

static int gimbal_send_frame(gimbal_t *gimbal,
                             uint8_t command1,
                             uint8_t command2,
                             uint8_t data1,
                             uint8_t data2) {
    pelco_d_frame_t frame;
    uint8_t raw[PELCO_D_FRAME_SIZE];
    char hex[PELCO_D_FRAME_SIZE * 3];

    if (!gimbal || !gimbal->is_open) {
        errno = ENOTCONN;
        return -1;
    }

    pelco_d_init_frame(&frame, gimbal->address, command1, command2, data1, data2);
    pelco_d_serialize(&frame, raw);
    pelco_d_format_hex(raw, sizeof(raw), hex, sizeof(hex));
    fprintf(stderr, "TX: %s\n", hex);
    return serial_port_write_all(&gimbal->serial, raw, sizeof(raw));
}

static int gimbal_read_frame(gimbal_t *gimbal,
                             uint8_t raw[PELCO_D_FRAME_SIZE],
                             pelco_d_frame_t *frame,
                             int timeout_ms) {
    if (serial_port_read_timeout(&gimbal->serial, raw,
                                 PELCO_D_FRAME_SIZE, timeout_ms) < 0) {
        return -1;
    }
    return pelco_d_parse(raw, frame);
}

/**
 * @brief   清空串口接收缓冲区中的残余数据
 *
 *          巡航/扫描期间硬件会持续发送位置上报帧，这些帧会堆积在
 *          串口接收缓冲区中。在发送查询命令之前调用本函数清空缓冲区，
 *          避免读到非期望的上报帧。
 *
 *          【重要】扫描期间硬件持续上报（每帧 ~29ms @2400bps），帧间
 *          几乎没有空闲。若不加限制逐字节等待 10ms 超时退出，则字节
 *          间隔仅 ~4ms 远小于 10ms 超时，函数将永远阻塞。
 *          因此增加 max_bytes 上限，最多丢弃约 20 帧后强制返回。
 */
void gimbal_drain_rx(gimbal_t *gimbal) {
    uint8_t discard;
    int max_bytes = 140;  /* 最多 20 帧 (7×20)，约 580ms */
    int count = 0;
    while (count < max_bytes &&
           serial_port_read_timeout(&gimbal->serial, &discard, 1, 10) == 0) {
        count++;
    }
}

static int gimbal_send_query(gimbal_t *gimbal,
                             uint8_t command2,
                             uint16_t *value_out,
                             uint8_t expected_reply_command2) {
    pelco_d_frame_t reply;
    uint8_t raw[PELCO_D_FRAME_SIZE];
    char hex[PELCO_D_FRAME_SIZE * 3];

    /*
     * 发送查询前先清空串口接收缓冲区，
     * 丢弃硬件在巡航/扫描期间主动发送的残余上报帧，
     * 避免读到非期望帧导致查询失败。
     */
    gimbal_drain_rx(gimbal);

    if (gimbal_send_frame(gimbal, 0x00, command2, 0x00, 0x00) < 0) {
        return -1;
    }

    /*
     * 循环读取，跳过硬件主动发送的非请求帧（如巡航期间的位置上报）。
     * 首次使用完整查询超时（等待真正的响应），
     * 后续使用短超时快速消耗残余帧。
     */
    for (int attempt = 0; attempt < 5; attempt++) {
        int timeout = (attempt == 0) ? GIMBAL_QUERY_TIMEOUT_MS : 100;

        if (gimbal_read_frame(gimbal, raw, &reply, timeout) != 0) {
            if (attempt == 0) {
                return -1;          /* 首次超时 → 真正的通信错误 */
            }
            break;                  /* 后续超时 → 残余数据已清空 */
        }

        pelco_d_format_hex(raw, sizeof(raw), hex, sizeof(hex));
        fprintf(stderr, "RX: %s\n", hex);

        if (reply.address == gimbal->address &&
            reply.command2 == expected_reply_command2) {
            *value_out = (uint16_t) ((reply.data1 << 8) | reply.data2);
            return 0;
        }

        fprintf(stderr, "RX: skip mismatch cmd2=0x%02X (want 0x%02X)\n",
                reply.command2, expected_reply_command2);
    }

    errno = EPROTO;
    return -1;
}

static uint16_t clamp_u16_from_double(double value, double min_value, double max_value) {
    if (value < min_value) {
        value = min_value;
    } else if (value > max_value) {
        value = max_value;
    }
    return (uint16_t) lround(value);
}

static int16_t clamp_i16_from_double(double value, double min_value, double max_value) {
    if (value < min_value) {
        value = min_value;
    } else if (value > max_value) {
        value = max_value;
    }
    return (int16_t) lround(value);
}

static uint16_t pan_deg_to_units(double degrees) {
    return clamp_u16_from_double(degrees * 100.0, 0.0, 0xFFFF);
}

static double pan_units_to_deg(uint16_t units) {
    return units / 100.0;
}

static int16_t tilt_deg_to_units(double degrees) {
    return clamp_i16_from_double(degrees * 100.0, -32768.0, 32767.0);
}

static double tilt_units_to_deg(int16_t units) {
    return units / 100.0;
}

static int gimbal_send_preset_command(gimbal_t *gimbal, uint8_t command2, uint8_t preset) {
    return gimbal_send_frame(gimbal, 0x00, command2, 0x00, preset);
}

static int gimbal_is_supported_preset(uint8_t preset) {
    /* 用户预置位 */
    if (preset >= 1  && preset <= 16) return 1;   /* 第一组 */
    if (preset >= 33 && preset <= 48) return 1;   /* 第二组 */

    /* 扫描相关 */
    if (preset >= 17 && preset <= 22) return 1;   /* 扫描限位+速度 */

    /* 巡航相关 */
    if (preset >= 70 && preset <= 72) return 1;   /* 巡航停留时间 */
    if (preset >= 83 && preset <= 84) return 1;   /* 巡航路线启动 */
    if (preset == 89)                 return 1;   /* 全角度扫描 */

    /* 其他特殊功能 */
    if (preset == 105)                return 1;   /* 自检 */
    if (preset == 110)                return 1;   /* 看守位 */
    if (preset == 111)                return 1;   /* 看守位超时 */
    if (preset == 125)                return 1;   /* 恢复默认 */

    return 0;
}

static int gimbal_position_reached(const gimbal_status_t *status, double pan_degrees, double tilt_degrees) {
    if (!status->pan_valid || !status->tilt_valid) {
        return 0;
    }

    return fabs(status->pan_degrees - pan_degrees) <= GIMBAL_POSITION_TOLERANCE_DEG &&
           fabs(status->tilt_degrees - tilt_degrees) <= GIMBAL_POSITION_TOLERANCE_DEG;
}

void gimbal_config_init_default(gimbal_config_t *config) {
    if (!config) {
        return;
    }

    config->device = "/dev/ttyS8";
    config->baudrate = 2400;
    config->address = 0x01;
}

int gimbal_init(gimbal_t *gimbal, const gimbal_config_t *config) {
    if (!gimbal || !config || !config->device) {
        errno = EINVAL;
        return -1;
    }

    return gimbal_open(gimbal, config->device, config->baudrate, config->address);
}

void gimbal_deinit(gimbal_t *gimbal) {
    gimbal_close(gimbal);
}

double gimbal_direction_max_speed_deg_per_sec(gimbal_direction_t direction) {
    switch (direction) {
        case GIMBAL_DIR_LEFT:
        case GIMBAL_DIR_RIGHT:
            return 14.3;
        case GIMBAL_DIR_UP:
        case GIMBAL_DIR_DOWN:
            return 1.7;
        default:
            return 0.0;
    }
}

int gimbal_speed_deg_per_sec_to_code(gimbal_direction_t direction, double degrees_per_sec, uint8_t *speed_code) {
    double max_speed = gimbal_direction_max_speed_deg_per_sec(direction);
    if (max_speed <= 0.0 || !speed_code || degrees_per_sec < 0.0) {
        errno = EINVAL;
        return -1;
    }
    if (degrees_per_sec > max_speed) {
        degrees_per_sec = max_speed;
    }

    *speed_code = (uint8_t) lround((degrees_per_sec / max_speed) * 0x3F);
    return 0;
}

int gimbal_move_deg_per_sec(gimbal_t *gimbal, gimbal_direction_t direction, double degrees_per_sec) {
    uint8_t speed = 0;
    if (gimbal_speed_deg_per_sec_to_code(direction, degrees_per_sec, &speed) < 0) {
        return -1;
    }
    return gimbal_move(gimbal, direction, speed);
}

int gimbal_move_to_position(gimbal_t *gimbal, double pan_degrees, double tilt_degrees) {
    gimbal_status_t status;
    const int max_attempts = GIMBAL_POSITION_TIMEOUT_MS / (GIMBAL_POSITION_SETTLE_DELAY_US / 1000);

    if (gimbal_set_pan(gimbal, pan_degrees) < 0) {
        return -1;
    }

    usleep(GIMBAL_POSITION_SETTLE_DELAY_US);

    if (gimbal_set_tilt(gimbal, tilt_degrees) < 0) {
        return -1;
    }

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        usleep(GIMBAL_POSITION_SETTLE_DELAY_US);
        if (gimbal_get_status(gimbal, &status) < 0) {
            continue;
        }
        if (gimbal_position_reached(&status, pan_degrees, tilt_degrees)) {
            return 0;
        }
    }

    errno = ETIMEDOUT;
    return -1;
}

int gimbal_initialize_presets(gimbal_t *gimbal,
                              const gimbal_preset_position_t *positions,
                              size_t count) {
    if (!gimbal || !positions || count == 0) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        if (gimbal_move_to_position(gimbal, positions[i].pan_degrees, positions[i].tilt_degrees) < 0) {
            return -1;
        }
        if (gimbal_preset_set(gimbal, positions[i].preset_id) < 0) {
            return -1;
        }
        usleep(GIMBAL_SYNC_DELAY_US);
    }

    return 0;
}

int gimbal_open(gimbal_t *gimbal, const char *device, int baudrate, uint8_t address) {
    memset(gimbal, 0, sizeof(*gimbal));
    gimbal->serial.fd = -1;
    gimbal->address = address;

    if (serial_port_open(&gimbal->serial, device, baudrate) < 0) {
        return -1;
    }

    gimbal->is_open = 1;
    return 0;
}

void gimbal_close(gimbal_t *gimbal) {
    if (!gimbal) {
        return;
    }
    serial_port_close(&gimbal->serial);
    gimbal->is_open = 0;
}

int gimbal_move(gimbal_t *gimbal, gimbal_direction_t direction, uint8_t speed) {
    switch (direction) {
        case GIMBAL_DIR_UP:
            return gimbal_send_frame(gimbal, 0x00, 0x08, 0x00, speed);
        case GIMBAL_DIR_DOWN:
            return gimbal_send_frame(gimbal, 0x00, 0x10, 0x00, speed);
        case GIMBAL_DIR_LEFT:
            return gimbal_send_frame(gimbal, 0x00, 0x04, speed, 0x00);
        case GIMBAL_DIR_RIGHT:
            return gimbal_send_frame(gimbal, 0x00, 0x02, speed, 0x00);
        default:
            errno = EINVAL;
            return -1;
    }
}

int gimbal_stop(gimbal_t *gimbal) {
    return gimbal_send_frame(gimbal, 0x00, 0x00, 0x00, 0x00);
}

int gimbal_preset_set(gimbal_t *gimbal, uint8_t preset) {
    return gimbal_send_preset_command(gimbal, 0x03, preset);
}

int gimbal_preset_call(gimbal_t *gimbal, uint8_t preset) {
    if (!gimbal_is_supported_preset(preset)) {
        fprintf(stderr, "invalid preset call: %u "
                "(supported: 1-16, 17-22, 33-48, 70-72, 83-84, 89, 105, 110-111, 125)\n",
                preset);
        errno = EINVAL;
        return -1;
    }

    return gimbal_send_preset_command(gimbal, 0x07, preset);
}

int gimbal_set_home(gimbal_t *gimbal) {
    return gimbal_preset_call(gimbal, 1);
}

int gimbal_self_check_enable(gimbal_t *gimbal, int enable) {
    return gimbal_send_preset_command(gimbal, enable ? 0x07 : 0x03, 105);
}

int gimbal_aux_set(gimbal_t *gimbal, int enable) {
    return gimbal_send_frame(gimbal, 0x00, enable ? 0x09 : 0x0B, 0x00, 0x00);
}

int gimbal_scan_set_left_limit(gimbal_t *gimbal, double pan_degrees, double tilt_degrees) {
    if (gimbal_move_to_position(gimbal, pan_degrees, tilt_degrees) < 0) {
        return -1;
    }

    return gimbal_preset_set(gimbal, 17);
}

int gimbal_scan_set_right_limit(gimbal_t *gimbal, double pan_degrees, double tilt_degrees) {
    if (gimbal_move_to_position(gimbal, pan_degrees, tilt_degrees) < 0) {
        return -1;
    }

    return gimbal_preset_set(gimbal, 18);
}

int gimbal_scan_start(gimbal_t *gimbal) {
    return gimbal_preset_call(gimbal, 19);
}

int gimbal_scan_set_speed(gimbal_t *gimbal, uint8_t speed_code) {
    if (gimbal_preset_set(gimbal, 22) < 0) {
        return -1;
    }

    /* 等待硬件进入扫描速度编程模式 */
    usleep(GIMBAL_SYNC_DELAY_US);

    return gimbal_preset_set(gimbal, speed_code);
}

int gimbal_scan_start_max(gimbal_t *gimbal) {
    return gimbal_preset_call(gimbal, 89);
}

int gimbal_cruise_set_dwell(gimbal_t *gimbal, uint8_t mode) {
    uint8_t preset;
    switch (mode) {
        case 0:
            preset = 70;
            break;
        case 1:
            preset = 71;
            break;
        case 2:
            preset = 72;
            break;
        default:
            fprintf(stderr, "invalid cruise dwell mode: %u (expected 0, 1, or 2)\n", mode);
            errno = EINVAL;
            return -1;
    }
    return gimbal_preset_call(gimbal, preset);
}

int gimbal_cruise_start_route1(gimbal_t *gimbal) {
    return gimbal_preset_call(gimbal, 83);
}

int gimbal_cruise_start_route2(gimbal_t *gimbal) {
    return gimbal_preset_call(gimbal, 84);
}

int gimbal_cruise_start(gimbal_t *gimbal, uint8_t route_id) {
    if (route_id == 1) {
        return gimbal_cruise_start_route1(gimbal);
    }
    if (route_id == 2) {
        return gimbal_cruise_start_route2(gimbal);
    }
    errno = EINVAL;
    return -1;
}

int gimbal_guard_enable(gimbal_t *gimbal, uint8_t preset) {
    if (preset != 1) {
        errno = EINVAL;
        return -1;
    }
    return gimbal_preset_set(gimbal, 110);
}

int gimbal_guard_disable(gimbal_t *gimbal) {
    return gimbal_preset_call(gimbal, 110);
}

int gimbal_guard_set_timeout(gimbal_t *gimbal, uint8_t seconds) {
    if (seconds < 30 || seconds > 250) {
        errno = EINVAL;
        return -1;
    }
    return gimbal_preset_call(gimbal, 111);
}

int gimbal_clear_presets(gimbal_t *gimbal) {
    return gimbal_send_frame(gimbal, 0x00, 0x07, 0x00, 0x78);
}

int gimbal_restore_defaults(gimbal_t *gimbal) {
    return gimbal_preset_call(gimbal, 125);
}

int gimbal_query_pan(gimbal_t *gimbal, double *degrees_out) {
    uint16_t units = 0;
    if (gimbal_send_query(gimbal, 0x51, &units, 0x59) < 0) {
        return -1;
    }
    *degrees_out = pan_units_to_deg(units);
    return 0;
}

int gimbal_query_tilt(gimbal_t *gimbal, double *degrees_out) {
    uint16_t units = 0;
    if (gimbal_send_query(gimbal, 0x53, &units, 0x5B) < 0) {
        return -1;
    }
    *degrees_out = tilt_units_to_deg((int16_t) units);
    return 0;
}

int gimbal_set_pan(gimbal_t *gimbal, double degrees) {
    if (degrees < GIMBAL_PAN_MIN_DEGREES || degrees > GIMBAL_PAN_MAX_DEGREES) {
        fprintf(stderr, "pan angle out of range: %.2f (expected %.2f~%.2f)\n",
                degrees, GIMBAL_PAN_MIN_DEGREES, GIMBAL_PAN_MAX_DEGREES);
        errno = ERANGE;
        return -1;
    }

    uint16_t units = pan_deg_to_units(degrees);
    return gimbal_send_frame(gimbal, 0x00, 0x4B, (uint8_t) (units >> 8), (uint8_t) units);
}

int gimbal_set_tilt(gimbal_t *gimbal, double degrees) {
    if (degrees < GIMBAL_TILT_MIN_DEGREES || degrees > GIMBAL_TILT_MAX_DEGREES) {
        fprintf(stderr, "tilt angle out of range: %.2f (expected %.2f~%.2f)\n",
                degrees, GIMBAL_TILT_MIN_DEGREES, GIMBAL_TILT_MAX_DEGREES);
        errno = ERANGE;
        return -1;
    }

    int16_t units = tilt_deg_to_units(degrees);
    uint16_t raw_units = (uint16_t) units;
    return gimbal_send_frame(gimbal, 0x00, 0x4D, (uint8_t) (raw_units >> 8), (uint8_t) raw_units);
}

int gimbal_get_status(gimbal_t *gimbal, gimbal_status_t *status) {
    memset(status, 0, sizeof(*status));

    if (gimbal_query_pan(gimbal, &status->pan_degrees) == 0) {
        status->pan_valid = 1;
    }

    usleep(GIMBAL_SYNC_DELAY_US);

    if (gimbal_query_tilt(gimbal, &status->tilt_degrees) == 0) {
        status->tilt_valid = 1;
    }

    return (status->pan_valid || status->tilt_valid) ? 0 : -1;
}