#ifndef SC_HARDWARE_H
#define SC_HARDWARE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Hardware discovery — scan for Arduino, STM32, RPi
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_hardware_info {
    char board_name[64];
    char board_type[32];
    char serial_port[128];
    bool detected;
} sc_hardware_info_t;

/**
 * Discover connected hardware (Arduino serial, STM32 via probe-rs, RPi GPIO).
 * Caller provides pre-allocated results array; count is in/out.
 * Returns SC_OK on success.
 */
sc_error_t sc_hardware_discover(sc_allocator_t *alloc,
    sc_hardware_info_t *results,
    size_t *count);

#endif /* SC_HARDWARE_H */
