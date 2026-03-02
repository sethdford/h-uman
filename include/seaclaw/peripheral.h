#ifndef SC_PERIPHERAL_H
#define SC_PERIPHERAL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Peripheral types — hardware boards, GPIO, flash
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_peripheral_error {
    SC_PERIPHERAL_ERR_NONE = 0,
    SC_PERIPHERAL_ERR_NOT_CONNECTED,
    SC_PERIPHERAL_ERR_IO,
    SC_PERIPHERAL_ERR_FLASH_FAILED,
    SC_PERIPHERAL_ERR_TIMEOUT,
    SC_PERIPHERAL_ERR_INVALID_ADDRESS,
    SC_PERIPHERAL_ERR_PERMISSION_DENIED,
    SC_PERIPHERAL_ERR_DEVICE_NOT_FOUND,
    SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION,
} sc_peripheral_error_t;

typedef struct sc_peripheral_capabilities {
    const char *board_name;
    size_t board_name_len;
    const char *board_type;
    size_t board_type_len;
    const char *gpio_pins;      /* e.g. "0,1,2,3" or "" */
    size_t gpio_pins_len;
    uint32_t flash_size_kb;
    bool has_serial;
    bool has_gpio;
    bool has_flash;
    bool has_adc;
} sc_peripheral_capabilities_t;

typedef struct sc_gpio_read_result {
    sc_peripheral_error_t err;
    uint8_t value;  /* valid when err == SC_PERIPHERAL_ERR_NONE */
} sc_gpio_read_result_t;

typedef struct sc_gpio_write_result {
    sc_peripheral_error_t err;
} sc_gpio_write_result_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Peripheral vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_peripheral_vtable;

typedef struct sc_peripheral {
    void *ctx;
    const struct sc_peripheral_vtable *vtable;
} sc_peripheral_t;

typedef struct sc_peripheral_vtable {
    const char *(*name)(void *ctx);
    const char *(*board_type)(void *ctx);
    bool (*health_check)(void *ctx);
    sc_peripheral_error_t (*init_peripheral)(void *ctx);
    sc_peripheral_error_t (*read)(void *ctx, uint32_t addr, uint8_t *out_value);
    sc_peripheral_error_t (*write)(void *ctx, uint32_t addr, uint8_t data);
    sc_peripheral_error_t (*flash)(void *ctx, const char *firmware_path, size_t path_len);
    sc_peripheral_capabilities_t (*capabilities)(void *ctx);
    void (*destroy)(void *ctx, sc_allocator_t *alloc);
} sc_peripheral_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory and config
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_peripheral_config {
    const char *serial_port;   /* for arduino */
    size_t serial_port_len;
    const char *chip;         /* for stm32, e.g. STM32F401RETx */
    size_t chip_len;
} sc_peripheral_config_t;

/**
 * Create peripheral by type ("arduino", "stm32", "rpi").
 * Returns SC_OK and fills *out on success; SC_ERR_NOT_SUPPORTED for unknown type.
 */
sc_error_t sc_peripheral_create(sc_allocator_t *alloc,
    const char *type, size_t type_len,
    const sc_peripheral_config_t *config,
    sc_peripheral_t *out);

/* Implementation-specific constructors (for factory use) */
sc_peripheral_t sc_arduino_peripheral_create(sc_allocator_t *alloc,
    const char *serial_port, size_t serial_port_len);
sc_peripheral_t sc_stm32_peripheral_create(sc_allocator_t *alloc,
    const char *chip, size_t chip_len);
sc_peripheral_t sc_rpi_peripheral_create(sc_allocator_t *alloc);

#endif /* SC_PERIPHERAL_H */
