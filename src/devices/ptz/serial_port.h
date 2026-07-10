#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
} serial_port_t;

int serial_port_open(serial_port_t *port, const char *device, int baudrate);
void serial_port_close(serial_port_t *port);
int serial_port_write_all(serial_port_t *port, const uint8_t *data, size_t len);
int serial_port_read_timeout(serial_port_t *port, uint8_t *buffer, size_t len, int timeout_ms);

#endif
