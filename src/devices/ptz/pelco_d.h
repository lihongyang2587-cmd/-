#ifndef PELCO_D_H
#define PELCO_D_H

#include <stddef.h>
#include <stdint.h>

#define PELCO_D_FRAME_SIZE 7

typedef struct {
    uint8_t sync;
    uint8_t address;
    uint8_t command1;
    uint8_t command2;
    uint8_t data1;
    uint8_t data2;
    uint8_t checksum;
} pelco_d_frame_t;

void pelco_d_init_frame(pelco_d_frame_t *frame,
                        uint8_t address,
                        uint8_t command1,
                        uint8_t command2,
                        uint8_t data1,
                        uint8_t data2);
uint8_t pelco_d_checksum(uint8_t address,
                         uint8_t command1,
                         uint8_t command2,
                         uint8_t data1,
                         uint8_t data2);
void pelco_d_serialize(const pelco_d_frame_t *frame, uint8_t out[PELCO_D_FRAME_SIZE]);
int pelco_d_parse(const uint8_t raw[PELCO_D_FRAME_SIZE], pelco_d_frame_t *frame);
void pelco_d_format_hex(const uint8_t *data, size_t len, char *buffer, size_t buffer_size);

#endif
