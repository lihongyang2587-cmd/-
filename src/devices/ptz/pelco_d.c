#include "pelco_d.h"

#include <stdio.h>

void pelco_d_init_frame(pelco_d_frame_t *frame,
                        uint8_t address,
                        uint8_t command1,
                        uint8_t command2,
                        uint8_t data1,
                        uint8_t data2) {
    frame->sync = 0xFF;
    frame->address = address;
    frame->command1 = command1;
    frame->command2 = command2;
    frame->data1 = data1;
    frame->data2 = data2;
    frame->checksum = pelco_d_checksum(address, command1, command2, data1, data2);
}

uint8_t pelco_d_checksum(uint8_t address,
                         uint8_t command1,
                         uint8_t command2,
                         uint8_t data1,
                         uint8_t data2) {
    return (uint8_t) (address + command1 + command2 + data1 + data2);
}

void pelco_d_serialize(const pelco_d_frame_t *frame, uint8_t out[PELCO_D_FRAME_SIZE]) {
    out[0] = frame->sync;
    out[1] = frame->address;
    out[2] = frame->command1;
    out[3] = frame->command2;
    out[4] = frame->data1;
    out[5] = frame->data2;
    out[6] = frame->checksum;
}

int pelco_d_parse(const uint8_t raw[PELCO_D_FRAME_SIZE], pelco_d_frame_t *frame) {
    if (raw[0] != 0xFF) {
        return -1;
    }

    uint8_t checksum = pelco_d_checksum(raw[1], raw[2], raw[3], raw[4], raw[5]);
    if (checksum != raw[6]) {
        return -2;
    }

    frame->sync = raw[0];
    frame->address = raw[1];
    frame->command1 = raw[2];
    frame->command2 = raw[3];
    frame->data1 = raw[4];
    frame->data2 = raw[5];
    frame->checksum = raw[6];
    return 0;
}

void pelco_d_format_hex(const uint8_t *data, size_t len, char *buffer, size_t buffer_size) {
    size_t offset = 0;
    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    for (size_t i = 0; i < len; ++i) {
        int written = snprintf(buffer + offset,
                               buffer_size - offset,
                               (i + 1 == len) ? "%02X" : "%02X ",
                               data[i]);
        if (written < 0 || (size_t) written >= buffer_size - offset) {
            break;
        }
        offset += (size_t) written;
    }
}
