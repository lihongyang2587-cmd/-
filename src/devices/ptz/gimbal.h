#ifndef GIMBAL_H
#define GIMBAL_H

#include <stddef.h>
#include <stdint.h>

#include "serial_port.h"

typedef struct {
    serial_port_t serial;
    uint8_t address;
    int is_open;
} gimbal_t;

typedef struct {
    const char *device;
    int baudrate;
    uint8_t address;
} gimbal_config_t;

typedef enum {
    GIMBAL_DIR_UP,
    GIMBAL_DIR_DOWN,
    GIMBAL_DIR_LEFT,
    GIMBAL_DIR_RIGHT
} gimbal_direction_t;

typedef struct {
    double pan_degrees;
    double tilt_degrees;
    int pan_valid;
    int tilt_valid;
} gimbal_status_t;

typedef struct {
    uint8_t preset_id;
    double pan_degrees;
    double tilt_degrees;
} gimbal_preset_position_t;

void gimbal_config_init_default(gimbal_config_t *config);
int gimbal_init(gimbal_t *gimbal, const gimbal_config_t *config);
void gimbal_deinit(gimbal_t *gimbal);

int gimbal_open(gimbal_t *gimbal, const char *device, int baudrate, uint8_t address);
void gimbal_close(gimbal_t *gimbal);
void gimbal_drain_rx(gimbal_t *gimbal);

double gimbal_direction_max_speed_deg_per_sec(gimbal_direction_t direction);
int gimbal_speed_deg_per_sec_to_code(gimbal_direction_t direction, double degrees_per_sec, uint8_t *speed_code);
int gimbal_move(gimbal_t *gimbal, gimbal_direction_t direction, uint8_t speed);
int gimbal_move_deg_per_sec(gimbal_t *gimbal, gimbal_direction_t direction, double degrees_per_sec);
int gimbal_stop(gimbal_t *gimbal);

int gimbal_preset_set(gimbal_t *gimbal, uint8_t preset);
int gimbal_preset_call(gimbal_t *gimbal, uint8_t preset);

int gimbal_set_home(gimbal_t *gimbal);
int gimbal_self_check_enable(gimbal_t *gimbal, int enable);
int gimbal_aux_set(gimbal_t *gimbal, int enable);

int gimbal_scan_set_left_limit(gimbal_t *gimbal, double pan_degrees, double tilt_degrees);
int gimbal_scan_set_right_limit(gimbal_t *gimbal, double pan_degrees, double tilt_degrees);
int gimbal_scan_start(gimbal_t *gimbal);
int gimbal_scan_set_speed(gimbal_t *gimbal, uint8_t speed_code);
int gimbal_scan_start_max(gimbal_t *gimbal);

int gimbal_cruise_set_dwell(gimbal_t *gimbal, uint8_t mode);
int gimbal_cruise_start_route1(gimbal_t *gimbal);
int gimbal_cruise_start_route2(gimbal_t *gimbal);
int gimbal_cruise_start(gimbal_t *gimbal, uint8_t route_id);

int gimbal_guard_enable(gimbal_t *gimbal, uint8_t preset);
int gimbal_guard_disable(gimbal_t *gimbal);
int gimbal_guard_set_timeout(gimbal_t *gimbal, uint8_t seconds);

int gimbal_clear_presets(gimbal_t *gimbal);
int gimbal_restore_defaults(gimbal_t *gimbal);

int gimbal_query_pan(gimbal_t *gimbal, double *degrees_out);
int gimbal_query_tilt(gimbal_t *gimbal, double *degrees_out);
int gimbal_set_pan(gimbal_t *gimbal, double degrees);
int gimbal_set_tilt(gimbal_t *gimbal, double degrees);
int gimbal_move_to_position(gimbal_t *gimbal, double pan_degrees, double tilt_degrees);
int gimbal_initialize_presets(gimbal_t *gimbal,
                              const gimbal_preset_position_t *positions,
                              size_t count);
int gimbal_get_status(gimbal_t *gimbal, gimbal_status_t *status);

#endif