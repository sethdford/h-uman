#ifndef HU_HARDWARE_H
#define HU_HARDWARE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Hardware discovery — scan for Arduino, STM32, RPi
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_hardware_info {
    char board_name[64];
    char board_type[32];
    char serial_port[128];
    bool detected;
} hu_hardware_info_t;

/**
 * Discover connected hardware (Arduino serial, STM32 via probe-rs, RPi GPIO).
 * Caller provides pre-allocated results array; count is in/out.
 * Returns HU_OK on success.
 */
hu_error_t hu_hardware_discover(hu_allocator_t *alloc, hu_hardware_info_t *results, size_t *count);

#endif /* HU_HARDWARE_H */
