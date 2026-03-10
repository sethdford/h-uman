#ifndef HU_HEALTH_H
#define HU_HEALTH_H

#include "core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Component health
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_component_health {
    char status[32]; /* "ok", "error", "starting" */
    char updated_at[32];
    char last_ok[32];
    char last_error[256]; /* last error message if status=error */
    uint64_t restart_count;
} hu_component_health_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Health snapshot
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_health_snapshot {
    uint32_t pid;
    uint64_t uptime_seconds;
    hu_component_health_t *components; /* caller owns, may be NULL */
    size_t component_count;
} hu_health_snapshot_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Readiness
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_readiness_status {
    HU_READINESS_READY,
    HU_READINESS_NOT_READY,
} hu_readiness_status_t;

typedef struct hu_component_check {
    const char *name;
    bool healthy;
    const char *message; /* optional */
} hu_component_check_t;

typedef struct hu_readiness_result {
    hu_readiness_status_t status;
    hu_component_check_t *checks;
    size_t check_count;
} hu_readiness_result_t;

/* ──────────────────────────────────────────────────────────────────────────
 * API
 * ────────────────────────────────────────────────────────────────────────── */

void hu_health_mark_ok(const char *component);
void hu_health_mark_error(const char *component, const char *message);
void hu_health_bump_restart(const char *component);

void hu_health_snapshot(hu_health_snapshot_t *out);
hu_readiness_result_t hu_health_check_readiness(hu_allocator_t *alloc);

void hu_health_reset(void);

#endif /* HU_HEALTH_H */
