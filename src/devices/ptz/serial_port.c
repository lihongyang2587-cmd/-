#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#ifndef CRTSCTS
#ifdef CNEW_RTSCTS
#define CRTSCTS CNEW_RTSCTS
#else
#define CRTSCTS 0
#endif
#endif

static speed_t serial_baud_to_flag(int baudrate) {
    switch (baudrate) {
        case 2400:
            return B2400;
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        default:
            return 0;
    }
}

int serial_port_open(serial_port_t *port, const char *device, int baudrate) {
    struct termios tty;
    speed_t speed = serial_baud_to_flag(baudrate);
    if (speed == 0) {
        errno = EINVAL;
        return -1;
    }

    port->fd = open(device, O_RDWR | O_NOCTTY | O_SYNC);
    if (port->fd < 0) {
        return -1;
    }

    if (tcgetattr(port->fd, &tty) != 0) {
        close(port->fd);
        port->fd = -1;
        return -1;
    }

    cfmakeraw(&tty);
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(port->fd, TCSANOW, &tty) != 0) {
        close(port->fd);
        port->fd = -1;
        return -1;
    }

    if (tcflush(port->fd, TCIOFLUSH) != 0) {
        close(port->fd);
        port->fd = -1;
        return -1;
    }

    return 0;
}

void serial_port_close(serial_port_t *port) {
    if (port->fd >= 0) {
        close(port->fd);
        port->fd = -1;
    }
}

int serial_port_write_all(serial_port_t *port, const uint8_t *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t written = write(port->fd, data + total, len - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t) written;
    }

    return tcdrain(port->fd);
}

int serial_port_read_timeout(serial_port_t *port, uint8_t *buffer, size_t len, int timeout_ms) {
    size_t total = 0;

    while (total < len) {
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(port->fd, &readfds);

        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ready = select(port->fd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        ssize_t received = read(port->fd, buffer + total, len - total);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            errno = EIO;
            return -1;
        }
        total += (size_t) received;
    }

    return (int) total;
}
