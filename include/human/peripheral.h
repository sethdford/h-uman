#ifndef HU_PERIPHERAL_H
#define HU_PERIPHERAL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Peripheral types — hardware boards, GPIO, flash
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_peripheral_error {
    HU_PERIPHERAL_ERR_NONE = 0,
    HU_PERIPHERAL_ERR_NOT_CONNECTED,
    HU_PERIPHERAL_ERR_IO,
    HU_PERIPHERAL_ERR_FLASH_FAILED,
    HU_PERIPHERAL_ERR_TIMEOUT,
    HU_PERIPHERAL_ERR_INVALID_ADDRESS,
    HU_PERIPHERAL_ERR_PERMISSION_DENIED,
    HU_PERIPHERAL_ERR_DEVICE_NOT_FOUND,
    HU_PERIPHERAL_ERR_UNSUPPORTED_OPERATION,
} hu_peripheral_error_t;

typedef struct hu_peripheral_capabilities {
    const char *board_name;
    size_t board_name_len;
    const char *board_type;
    size_t board_type_len;
    const char *gpio_pins; /* e.g. "0,1,2,3" or "" */
    size_t gpio_pins_len;
    uint32_t flash_size_kb;
    bool has_serial;
    bool has_gpio;
    bool has_flash;
    bool has_adc;
} hu_peripheral_capabilities_t;

typedef struct hu_gpio_read_result {
    hu_peripheral_error_t err;
    uint8_t value; /* valid when err == HU_PERIPHERAL_ERR_NONE */
} hu_gpio_read_result_t;

typedef struct hu_gpio_write_result {
    hu_peripheral_error_t err;
} hu_gpio_write_result_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Peripheral vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_peripheral_vtable;

typedef struct hu_peripheral {
    void *ctx;
    const struct hu_peripheral_vtable *vtable;
} hu_peripheral_t;

typedef struct hu_peripheral_vtable {
    const char *(*name)(void *ctx);
    const char *(*board_type)(void *ctx);
    bool (*health_check)(void *ctx);
    hu_peripheral_error_t (*init_peripheral)(void *ctx);
    hu_peripheral_error_t (*read)(void *ctx, uint32_t addr, uint8_t *out_value);
    hu_peripheral_error_t (*write)(void *ctx, uint32_t addr, uint8_t data);
    hu_peripheral_error_t (*flash)(void *ctx, const char *firmware_path, size_t path_len);
    hu_peripheral_capabilities_t (*capabilities)(void *ctx);
    void (*destroy)(void *ctx, hu_allocator_t *alloc);
} hu_peripheral_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory and config
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_peripheral_config {
    const char *serial_port; /* for arduino */
    size_t serial_port_len;
    const char *chip; /* for stm32, e.g. STM32F401RETx */
    size_t chip_len;
} hu_peripheral_config_t;

/**
 * Create peripheral by type ("arduino", "stm32", "rpi").
 * Returns HU_OK and fills *out on success; HU_ERR_NOT_SUPPORTED for unknown type.
 */
hu_error_t hu_peripheral_create(hu_allocator_t *alloc, const char *type, size_t type_len,
                                const hu_peripheral_config_t *config, hu_peripheral_t *out);

/* Implementation-specific constructors (for factory use) */
hu_peripheral_t hu_arduino_peripheral_create(hu_allocator_t *alloc, const char *serial_port,
                                             size_t serial_port_len);
hu_peripheral_t hu_stm32_peripheral_create(hu_allocator_t *alloc, const char *chip,
                                           size_t chip_len);
hu_peripheral_t hu_rpi_peripheral_create(hu_allocator_t *alloc);

#endif /* HU_PERIPHERAL_H */
