#include "seaclaw/peripheral.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef SC_IS_TEST
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#endif
#endif

typedef struct sc_arduino_ctx {
    sc_allocator_t *alloc;
    char *serial_port;    /* owned */
    size_t serial_port_len;
    char board_name[64];
    bool connected;
    int fd;              /* serial fd, -1 when closed */
    uint32_t msg_id;
} sc_arduino_ctx_t;

static const char *impl_name(void *ctx) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    return s->board_name[0] ? s->board_name : "arduino";
}

static const char *impl_board_type(void *ctx) {
    (void)ctx;
    return "arduino-uno";
}

static bool impl_health_check(void *ctx) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    return s->connected;
}

static void set_default_board_name(sc_arduino_ctx_t *s) {
    strncpy(s->board_name, "arduino-uno", sizeof(s->board_name) - 1);
    s->board_name[sizeof(s->board_name) - 1] = '\0';
}

static sc_peripheral_error_t impl_init(void *ctx) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    s->connected = false;

#ifndef SC_IS_TEST
#ifdef __linux__
    if (!s->serial_port || s->serial_port_len == 0) return SC_PERIPHERAL_ERR_DEVICE_NOT_FOUND;
    s->fd = open(s->serial_port, O_RDWR | O_NOCTTY);
    if (s->fd < 0) return SC_PERIPHERAL_ERR_DEVICE_NOT_FOUND;

    struct termios tty;
    if (tcgetattr(s->fd, &tty) != 0) {
        close(s->fd);
        s->fd = -1;
        return SC_PERIPHERAL_ERR_IO;
    }
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;
    if (tcsetattr(s->fd, TCSANOW, &tty) != 0) {
        close(s->fd);
        s->fd = -1;
        return SC_PERIPHERAL_ERR_IO;
    }

    char handshake[] = "{\"cmd\":\"handshake\"}\n";
    ssize_t wrote = write(s->fd, handshake, sizeof(handshake) - 1);
    if (wrote < 0) {
        close(s->fd);
        s->fd = -1;
        return SC_PERIPHERAL_ERR_IO;
    }
    char resp[256];
    ssize_t n = read(s->fd, resp, sizeof(resp) - 1);
    if (n > 0) {
        resp[n] = '\0';
        if (strstr(resp, "\"ok\":true") != NULL) {
            const char *board = strstr(resp, "\"board\":\"");
            if (board) {
                board += 9;
                const char *end = strchr(board, '"');
                size_t len = end ? (size_t)(end - board) : 0;
                if (len > 0 && len < sizeof(s->board_name) - 1) {
                    memcpy(s->board_name, board, len);
                    s->board_name[len] = '\0';
                } else {
                    set_default_board_name(s);
                }
            } else {
                set_default_board_name(s);
            }
        } else {
            set_default_board_name(s);
        }
    } else {
        set_default_board_name(s);
    }
    s->connected = true;
    return SC_PERIPHERAL_ERR_NONE;
#endif
#ifdef __APPLE__
    if (!s->serial_port || s->serial_port_len == 0) return SC_PERIPHERAL_ERR_DEVICE_NOT_FOUND;
    s->fd = open(s->serial_port, O_RDWR | O_NOCTTY);
    if (s->fd < 0) return SC_PERIPHERAL_ERR_DEVICE_NOT_FOUND;

    struct termios tty;
    if (tcgetattr(s->fd, &tty) != 0) {
        close(s->fd);
        s->fd = -1;
        return SC_PERIPHERAL_ERR_IO;
    }
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;
    if (tcsetattr(s->fd, TCSANOW, &tty) != 0) {
        close(s->fd);
        s->fd = -1;
        return SC_PERIPHERAL_ERR_IO;
    }

    char handshake[] = "{\"cmd\":\"handshake\"}\n";
    ssize_t wrote = write(s->fd, handshake, sizeof(handshake) - 1);
    if (wrote < 0) {
        close(s->fd);
        s->fd = -1;
        return SC_PERIPHERAL_ERR_IO;
    }
    char resp[256];
    ssize_t n = read(s->fd, resp, sizeof(resp) - 1);
    if (n > 0) {
        resp[n] = '\0';
        if (strstr(resp, "\"ok\":true") != NULL) {
            const char *board = strstr(resp, "\"board\":\"");
            if (board) {
                board += 9;
                const char *end = strchr(board, '"');
                size_t len = end ? (size_t)(end - board) : 0;
                if (len > 0 && len < sizeof(s->board_name) - 1) {
                    memcpy(s->board_name, board, len);
                    s->board_name[len] = '\0';
                } else {
                    set_default_board_name(s);
                }
            } else {
                set_default_board_name(s);
            }
        } else {
            set_default_board_name(s);
        }
    } else {
        set_default_board_name(s);
    }
    s->connected = true;
    return SC_PERIPHERAL_ERR_NONE;
#endif
#if !defined(__linux__) && !defined(__APPLE__)
    return SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    set_default_board_name(s);
    s->connected = true;
    return SC_PERIPHERAL_ERR_NONE;
#endif
}

static sc_peripheral_error_t impl_read(void *ctx, uint32_t addr, uint8_t *out_value) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    if (!out_value) return SC_PERIPHERAL_ERR_INVALID_ADDRESS;
    if (!s->connected) return SC_PERIPHERAL_ERR_NOT_CONNECTED;

#ifndef SC_IS_TEST
    char cmd[128];
    int n = snprintf(cmd, sizeof(cmd), "{\"cmd\":\"read\",\"addr\":%u}\n", (unsigned)addr);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) return SC_PERIPHERAL_ERR_IO;
    ssize_t wrote = write(s->fd, cmd, (size_t)n);
    if (wrote < 0) return SC_PERIPHERAL_ERR_IO;

    char resp[256];
    ssize_t nr = read(s->fd, resp, sizeof(resp) - 1);
    if (nr <= 0) return SC_PERIPHERAL_ERR_TIMEOUT;
    resp[nr] = '\0';

    const char *val = strstr(resp, "\"result\":");
    if (!val) val = strstr(resp, "\"value\":");
    if (!val) return SC_PERIPHERAL_ERR_IO;
    val += 9;
    *out_value = (uint8_t)atoi(val);
    return SC_PERIPHERAL_ERR_NONE;
#else
    (void)addr;
    *out_value = 0;
    return SC_PERIPHERAL_ERR_NONE;
#endif
}

static sc_peripheral_error_t impl_write(void *ctx, uint32_t addr, uint8_t data) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    if (!s->connected) return SC_PERIPHERAL_ERR_NOT_CONNECTED;

#ifndef SC_IS_TEST
    char cmd[128];
    int n = snprintf(cmd, sizeof(cmd), "{\"cmd\":\"write\",\"addr\":%u,\"data\":%u}\n",
        (unsigned)addr, (unsigned)data);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) return SC_PERIPHERAL_ERR_IO;
    ssize_t wrote = write(s->fd, cmd, (size_t)n);
    if (wrote < 0) return SC_PERIPHERAL_ERR_IO;

    char resp[128];
    ssize_t nr = read(s->fd, resp, sizeof(resp) - 1);
    if (nr <= 0) return SC_PERIPHERAL_ERR_TIMEOUT;
    resp[nr] = '\0';
    if (strstr(resp, "\"ok\":true") == NULL) return SC_PERIPHERAL_ERR_IO;
    return SC_PERIPHERAL_ERR_NONE;
#else
    (void)addr;
    (void)data;
    return SC_PERIPHERAL_ERR_NONE;
#endif
}

static sc_peripheral_error_t impl_flash(void *ctx, const char *firmware_path, size_t path_len) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    if (!s->connected) return SC_PERIPHERAL_ERR_NOT_CONNECTED;
    if (!firmware_path || path_len == 0) return SC_PERIPHERAL_ERR_FLASH_FAILED;

#ifndef SC_IS_TEST
    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd), "avrdude -p atmega328p -c arduino -P %.*s -U flash:w:%.*s:i -q",
        (int)s->serial_port_len, s->serial_port, (int)path_len, firmware_path);
    if (n <= 0 || n >= (int)sizeof(cmd)) return SC_PERIPHERAL_ERR_FLASH_FAILED;
    int r = system(cmd);
    if (r != 0) return SC_PERIPHERAL_ERR_FLASH_FAILED;
    return SC_PERIPHERAL_ERR_NONE;
#else
    (void)firmware_path;
    (void)path_len;
    return SC_PERIPHERAL_ERR_NONE;
#endif
}

static void impl_destroy(void *ctx, sc_allocator_t *alloc) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    if (s->serial_port)
        sc_str_free(alloc, s->serial_port);
    alloc->free(alloc->ctx, s, sizeof(sc_arduino_ctx_t));
}

static sc_peripheral_capabilities_t impl_capabilities(void *ctx) {
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)ctx;
    const char *name = s->board_name[0] ? s->board_name : "arduino-uno";
    sc_peripheral_capabilities_t cap = {
        .board_name = name,
        .board_name_len = strlen(name),
        .board_type = "arduino-uno",
        .board_type_len = 12,
        .gpio_pins = "0-13",
        .gpio_pins_len = 5,
        .flash_size_kb = 32,
        .has_serial = true,
        .has_gpio = true,
        .has_flash = true,
        .has_adc = false,
    };
    return cap;
}

static const sc_peripheral_vtable_t arduino_vtable = {
    .name = impl_name,
    .board_type = impl_board_type,
    .health_check = impl_health_check,
    .init_peripheral = impl_init,
    .read = impl_read,
    .write = impl_write,
    .flash = impl_flash,
    .capabilities = impl_capabilities,
    .destroy = impl_destroy,
};

sc_peripheral_t sc_arduino_peripheral_create(sc_allocator_t *alloc,
    const char *serial_port, size_t serial_port_len)
{
    if (!alloc || !serial_port || serial_port_len == 0) {
        return (sc_peripheral_t){ .ctx = NULL, .vtable = NULL };
    }
    sc_arduino_ctx_t *s = (sc_arduino_ctx_t *)alloc->alloc(alloc->ctx, sizeof(sc_arduino_ctx_t));
    if (!s) return (sc_peripheral_t){ .ctx = NULL, .vtable = NULL };

    char *port_copy = sc_strndup(alloc, serial_port, serial_port_len);
    if (!port_copy) {
        alloc->free(alloc->ctx, s, sizeof(sc_arduino_ctx_t));
        return (sc_peripheral_t){ .ctx = NULL, .vtable = NULL };
    }

    s->alloc = alloc;
    s->serial_port = port_copy;
    s->serial_port_len = serial_port_len;
    s->board_name[0] = '\0';
    s->connected = false;
    s->fd = -1;
    s->msg_id = 0;

    return (sc_peripheral_t){ .ctx = s, .vtable = &arduino_vtable };
}
