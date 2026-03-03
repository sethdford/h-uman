#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/peripheral.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef SC_IS_TEST
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif
#endif

typedef struct sc_rpi_ctx {
    sc_allocator_t *alloc;
    char board_name[64];
    bool connected;
} sc_rpi_ctx_t;

static const char *impl_name(void *ctx) {
    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)ctx;
    return s->board_name[0] ? s->board_name : "rpi-gpio";
}

static const char *impl_board_type(void *ctx) {
    (void)ctx;
    return "rpi-gpio";
}

static bool impl_health_check(void *ctx) {
    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)ctx;
    return s->connected;
}

static sc_peripheral_error_t impl_init(void *ctx) {
    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)ctx;
    s->connected = false;

#ifndef SC_IS_TEST
#ifdef __linux__
    int fd = open("/sys/class/gpio", O_RDONLY);
    if (fd < 0)
        return SC_PERIPHERAL_ERR_DEVICE_NOT_FOUND;
    close(fd);

    fd = open("/proc/device-tree/model", O_RDONLY);
    if (fd >= 0) {
        char buf[64];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\0')) {
                buf[--n] = '\0';
            }
            strncpy(s->board_name, buf, sizeof(s->board_name) - 1);
            s->board_name[sizeof(s->board_name) - 1] = '\0';
        } else {
            strncpy(s->board_name, "rpi-gpio", sizeof(s->board_name) - 1);
            s->board_name[sizeof(s->board_name) - 1] = '\0';
        }
    } else {
        strncpy(s->board_name, "rpi-gpio", sizeof(s->board_name) - 1);
        s->board_name[sizeof(s->board_name) - 1] = '\0';
    }
    s->connected = true;
    return SC_PERIPHERAL_ERR_NONE;
#else
    return SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    strncpy(s->board_name, "rpi-gpio", sizeof(s->board_name) - 1);
    s->board_name[sizeof(s->board_name) - 1] = '\0';
    s->connected = true;
    return SC_PERIPHERAL_ERR_NONE;
#endif
}

static sc_peripheral_error_t impl_read(void *ctx, uint32_t addr, uint8_t *out_value) {
    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)ctx;
    if (!out_value)
        return SC_PERIPHERAL_ERR_INVALID_ADDRESS;
    if (!s->connected)
        return SC_PERIPHERAL_ERR_NOT_CONNECTED;
    if (addr >= 64)
        return SC_PERIPHERAL_ERR_INVALID_ADDRESS;

#ifndef SC_IS_TEST
#ifdef __linux__
    char export_path[] = "/sys/class/gpio/export";
    char dir_path[64], val_path[64];
    snprintf(dir_path, sizeof(dir_path), "/sys/class/gpio/gpio%u/direction", (unsigned)addr);
    snprintf(val_path, sizeof(val_path), "/sys/class/gpio/gpio%u/value", (unsigned)addr);

    int fe = open(export_path, O_WRONLY);
    if (fe >= 0) {
        char pin[8];
        snprintf(pin, sizeof(pin), "%u", (unsigned)addr);
        write(fe, pin, strlen(pin));
        close(fe);
    }

    int fd = open(dir_path, O_WRONLY);
    if (fd >= 0) {
        write(fd, "in", 2);
        close(fd);
    }

    fd = open(val_path, O_RDONLY);
    if (fd < 0)
        return SC_PERIPHERAL_ERR_IO;
    char buf[4];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0)
        return SC_PERIPHERAL_ERR_IO;
    *out_value = (buf[0] == '1') ? 1 : 0;
    return SC_PERIPHERAL_ERR_NONE;
#else
    return SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    (void)addr;
    *out_value = 0;
    return SC_PERIPHERAL_ERR_NONE;
#endif
}

static sc_peripheral_error_t impl_write(void *ctx, uint32_t addr, uint8_t data) {
    (void)data;
    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)ctx;
    if (!s->connected)
        return SC_PERIPHERAL_ERR_NOT_CONNECTED;
    if (addr >= 64)
        return SC_PERIPHERAL_ERR_INVALID_ADDRESS;

#ifndef SC_IS_TEST
#ifdef __linux__
    char export_path[] = "/sys/class/gpio/export";
    char dir_path[64], val_path[64];
    snprintf(dir_path, sizeof(dir_path), "/sys/class/gpio/gpio%u/direction", (unsigned)addr);
    snprintf(val_path, sizeof(val_path), "/sys/class/gpio/gpio%u/value", (unsigned)addr);

    int fe = open(export_path, O_WRONLY);
    if (fe >= 0) {
        char pin[8];
        snprintf(pin, sizeof(pin), "%u", (unsigned)addr);
        write(fe, pin, strlen(pin));
        close(fe);
    }

    int fd = open(dir_path, O_WRONLY);
    if (fd >= 0) {
        write(fd, "out", 3);
        close(fd);
    }

    fd = open(val_path, O_WRONLY);
    if (fd < 0)
        return SC_PERIPHERAL_ERR_IO;
    const char *val = (data != 0) ? "1" : "0";
    write(fd, val, 1);
    close(fd);
    return SC_PERIPHERAL_ERR_NONE;
#else
    return SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    (void)addr;
    (void)data;
    return SC_PERIPHERAL_ERR_NONE;
#endif
}

static sc_peripheral_error_t impl_flash(void *ctx, const char *firmware_path, size_t path_len) {
    (void)ctx;
    (void)firmware_path;
    (void)path_len;
    return SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
}

static void impl_destroy(void *ctx, sc_allocator_t *alloc) {
    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)ctx;
    alloc->free(alloc->ctx, s, sizeof(sc_rpi_ctx_t));
}

static sc_peripheral_capabilities_t impl_capabilities(void *ctx) {
    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)ctx;
    const char *name = s->board_name[0] ? s->board_name : "rpi-gpio";
    sc_peripheral_capabilities_t cap = {
        .board_name = name,
        .board_name_len = strlen(name),
        .board_type = "rpi-gpio",
        .board_type_len = 9,
        .gpio_pins = "2-27",
        .gpio_pins_len = 5,
        .flash_size_kb = 0,
        .has_serial = false,
        .has_gpio = true,
        .has_flash = false,
        .has_adc = false,
    };
    return cap;
}

static const sc_peripheral_vtable_t rpi_vtable = {
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

sc_peripheral_t sc_rpi_peripheral_create(sc_allocator_t *alloc) {
    if (!alloc)
        return (sc_peripheral_t){.ctx = NULL, .vtable = NULL};

    sc_rpi_ctx_t *s = (sc_rpi_ctx_t *)alloc->alloc(alloc->ctx, sizeof(sc_rpi_ctx_t));
    if (!s)
        return (sc_peripheral_t){.ctx = NULL, .vtable = NULL};

    s->alloc = alloc;
    s->board_name[0] = '\0';
    s->connected = false;

    return (sc_peripheral_t){.ctx = s, .vtable = &rpi_vtable};
}
