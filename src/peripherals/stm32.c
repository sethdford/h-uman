#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/peripheral.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HU_IS_TEST
#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

typedef struct hu_stm32_ctx {
    hu_allocator_t *alloc;
    char *chip; /* e.g. STM32F401RETx, owned */
    size_t chip_len;
    char board_name[64];
    bool connected;
} hu_stm32_ctx_t;

#if !defined(_WIN32) && !defined(HU_IS_TEST)
static bool is_safe_path_len(const char *path, size_t len) {
    if (!path || len == 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (i + 1 < len && path[i] == '.' && path[i + 1] == '.')
            return false;
        char c = path[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '.' && c != '_' && c != '/' && c != '-' && c != '~')
            return false;
    }
    return true;
}
#endif

static const char *impl_name(void *ctx) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    return s->board_name[0] ? s->board_name : s->chip;
}

static const char *impl_board_type(void *ctx) {
    (void)ctx;
    return "nucleo";
}

static bool impl_health_check(void *ctx) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    return s->connected;
}

static hu_peripheral_error_t impl_init(void *ctx) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    s->connected = false;

#ifndef HU_IS_TEST
#ifndef _WIN32
    /* Constant string: no user input, safe from shell injection */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "probe-rs info 2>/dev/null");
    FILE *f = popen(cmd, "r");
    if (!f)
        return HU_PERIPHERAL_ERR_DEVICE_NOT_FOUND;

    char buf[512];
    size_t total = 0;
    while (fgets(buf, sizeof(buf), f) && total < sizeof(buf) - 1) {
        total += strlen(buf);
    }
    pclose(f);

    /* Constant string: no user input, safe from shell injection */
    if (system("probe-rs --version >/dev/null 2>&1") != 0)
        return HU_PERIPHERAL_ERR_DEVICE_NOT_FOUND;

    strncpy(s->board_name, s->chip, sizeof(s->board_name) - 1);
    s->board_name[sizeof(s->board_name) - 1] = '\0';
    s->connected = true;
    return HU_PERIPHERAL_ERR_NONE;
#else
    return HU_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    strncpy(s->board_name, s->chip ? s->chip : "STM32F401RETx", sizeof(s->board_name) - 1);
    s->board_name[sizeof(s->board_name) - 1] = '\0';
    s->connected = true;
    return HU_PERIPHERAL_ERR_NONE;
#endif
}

static hu_peripheral_error_t impl_read(void *ctx, uint32_t addr, uint8_t *out_value) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    if (!out_value)
        return HU_PERIPHERAL_ERR_INVALID_ADDRESS;
    if (!s->connected)
        return HU_PERIPHERAL_ERR_NOT_CONNECTED;

#ifndef HU_IS_TEST
#ifndef _WIN32
    if (!is_safe_path_len(s->chip, s->chip_len))
        return HU_PERIPHERAL_ERR_IO;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "probe-rs read --chip %.*s 0x%x 1 2>/dev/null", (int)s->chip_len,
             s->chip, (unsigned)addr);
    FILE *f = popen(cmd, "r");
    if (!f)
        return HU_PERIPHERAL_ERR_IO;
    char buf[16];
    if (!fgets(buf, sizeof(buf), f)) {
        pclose(f);
        return HU_PERIPHERAL_ERR_TIMEOUT;
    }
    pclose(f);
    *out_value = (uint8_t)strtoul(buf, NULL, 16);
    return HU_PERIPHERAL_ERR_NONE;
#else
    (void)addr;
    return HU_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    (void)addr;
    *out_value = 0;
    return HU_PERIPHERAL_ERR_NONE;
#endif
}

static hu_peripheral_error_t impl_write(void *ctx, uint32_t addr, uint8_t data) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    if (!s->connected)
        return HU_PERIPHERAL_ERR_NOT_CONNECTED;

#ifndef HU_IS_TEST
#ifndef _WIN32
    if (!is_safe_path_len(s->chip, s->chip_len))
        return HU_PERIPHERAL_ERR_IO;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "probe-rs write --chip %.*s 0x%x %02x 2>/dev/null", (int)s->chip_len,
             s->chip, (unsigned)addr, (unsigned)data);
    int r = system(cmd);
    if (r != 0)
        return HU_PERIPHERAL_ERR_IO;
    return HU_PERIPHERAL_ERR_NONE;
#else
    return HU_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    (void)addr;
    (void)data;
    return HU_PERIPHERAL_ERR_NONE;
#endif
}

static hu_peripheral_error_t impl_flash(void *ctx, const char *firmware_path, size_t path_len) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    if (!s->connected)
        return HU_PERIPHERAL_ERR_NOT_CONNECTED;
    if (!firmware_path || path_len == 0)
        return HU_PERIPHERAL_ERR_FLASH_FAILED;

#ifndef HU_IS_TEST
#ifndef _WIN32
    if (!is_safe_path_len(s->chip, s->chip_len) || !is_safe_path_len(firmware_path, path_len))
        return HU_PERIPHERAL_ERR_FLASH_FAILED;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "probe-rs download --chip %.*s %.*s 2>/dev/null", (int)s->chip_len,
             s->chip, (int)path_len, firmware_path);
    int r = system(cmd);
    if (r != 0)
        return HU_PERIPHERAL_ERR_FLASH_FAILED;
    return HU_PERIPHERAL_ERR_NONE;
#else
    return HU_PERIPHERAL_ERR_UNSUPPORTED_OPERATION;
#endif
#else
    (void)firmware_path;
    (void)path_len;
    return HU_PERIPHERAL_ERR_NONE;
#endif
}

static void impl_destroy(void *ctx, hu_allocator_t *alloc) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    if (s->chip)
        hu_str_free(alloc, s->chip);
    alloc->free(alloc->ctx, s, sizeof(hu_stm32_ctx_t));
}

static hu_peripheral_capabilities_t impl_capabilities(void *ctx) {
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)ctx;
    const char *name = s->board_name[0] ? s->board_name : s->chip;
    hu_peripheral_capabilities_t cap = {
        .board_name = name,
        .board_name_len = name ? strlen(name) : 0,
        .board_type = "nucleo",
        .board_type_len = 6,
        .gpio_pins = "",
        .gpio_pins_len = 0,
        .flash_size_kb = 512,
        .has_serial = true,
        .has_gpio = true,
        .has_flash = true,
        .has_adc = false,
    };
    return cap;
}

static const hu_peripheral_vtable_t stm32_vtable = {
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

hu_peripheral_t hu_stm32_peripheral_create(hu_allocator_t *alloc, const char *chip,
                                           size_t chip_len) {
    if (!alloc || !chip || chip_len == 0) {
        return (hu_peripheral_t){.ctx = NULL, .vtable = NULL};
    }
    hu_stm32_ctx_t *s = (hu_stm32_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_stm32_ctx_t));
    if (!s)
        return (hu_peripheral_t){.ctx = NULL, .vtable = NULL};

    char *chip_copy = hu_strndup(alloc, chip, chip_len);
    if (!chip_copy) {
        alloc->free(alloc->ctx, s, sizeof(hu_stm32_ctx_t));
        return (hu_peripheral_t){.ctx = NULL, .vtable = NULL};
    }

    s->alloc = alloc;
    s->chip = chip_copy;
    s->chip_len = chip_len;
    s->board_name[0] = '\0';
    s->connected = false;

    return (hu_peripheral_t){.ctx = s, .vtable = &stm32_vtable};
}
