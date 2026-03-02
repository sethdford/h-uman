#ifndef SC_HEALTH_H
#define SC_HEALTH_H

#include "core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Component health
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_component_health {
    char status[32];       /* "ok", "error", "starting" */
    char updated_at[32];
    char last_ok[32];
    char last_error[256];  /* last error message if status=error */
    uint64_t restart_count;
} sc_component_health_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Health snapshot
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_health_snapshot {
    uint32_t pid;
    uint64_t uptime_seconds;
    sc_component_health_t *components;  /* caller owns, may be NULL */
    size_t component_count;
} sc_health_snapshot_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Readiness
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_readiness_status {
    SC_READINESS_READY,
    SC_READINESS_NOT_READY,
} sc_readiness_status_t;

typedef struct sc_component_check {
    const char *name;
    bool healthy;
    const char *message;  /* optional */
} sc_component_check_t;

typedef struct sc_readiness_result {
    sc_readiness_status_t status;
    sc_component_check_t *checks;
    size_t check_count;
} sc_readiness_result_t;

/* ──────────────────────────────────────────────────────────────────────────
 * API
 * ────────────────────────────────────────────────────────────────────────── */

void sc_health_mark_ok(const char *component);
void sc_health_mark_error(const char *component, const char *message);
void sc_health_bump_restart(const char *component);

void sc_health_snapshot(sc_health_snapshot_t *out);
sc_readiness_result_t sc_health_check_readiness(sc_allocator_t *alloc);

void sc_health_reset(void);

#endif /* SC_HEALTH_H */
