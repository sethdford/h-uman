/* h-uman doctor WebSocket consumer — subscribes to the gateway's
 * event broadcast and streams formatted events to stdout + JSONL log.
 *
 * Spec: docs/plans/2026-05-27-doctor-ws-consumer/
 *
 * Two halves of the API:
 *  - PURE helpers (format_event_line, event_matches_filter): no I/O,
 *    fully unit-testable, no HU_IS_TEST gate needed.
 *  - hu_doctor_ws_watch: full TCP+WS loop, gated under HU_IS_TEST so
 *    unit tests don't actually open sockets.
 */

#ifndef HU_DOCTOR_WS_CONSUMER_H
#define HU_DOCTOR_WS_CONSUMER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_doctor_ws_config {
    const char *host;                /* default "127.0.0.1" */
    uint16_t port;                   /* default 3006 */
    const char *path;                /* default "/ws" */
    const char *event_filter;        /* NULL = all events; else comma-separated */
    const char *log_path;            /* NULL = auto-generate under ~/.human/logs/ */
    uint32_t max_reconnect_attempts; /* default 3 */
    bool quiet_stdout;               /* default false; true suppresses stdout (tests) */
} hu_doctor_ws_config_t;

/* Initialize a default config — caller can override individual fields. */
hu_doctor_ws_config_t hu_doctor_ws_config_default(void);

/* PURE: format one event into a one-line printable string. The line includes
 *   - [HH:MM:SS] local timestamp
 *   - event name
 *   - seq=<N>
 *   - a one-line summary of the payload (event-specific)
 *
 * Returns a heap string the CALLER owns (free via alloc->free). NULL on
 * format error.
 *
 * payload_json must be a complete JSON object string ("{...}"). Pass "{}"
 * if the source emitted no payload.
 *
 * Pure: no I/O, no globals, no time-of-day side effects beyond the
 * embedded timestamp (which uses local clock_gettime; pass now_epoch=0 to
 * accept current time, else use the fixed epoch for deterministic tests).
 */
char *hu_doctor_ws_format_event_line(hu_allocator_t *alloc, const char *event_name,
                                     const char *payload_json, uint64_t seq, int64_t now_epoch);

/* PURE: returns true iff `event_name` should be printed under `filter_csv`.
 *
 * filter_csv conventions:
 *   NULL or empty  -> match all events
 *   "chat,error"   -> exact match either "chat" or "error"
 *   "chat, error " -> whitespace trimmed around each token
 *
 * No glob, no negation in v1 (see design.md DECISION-3).
 */
bool hu_doctor_ws_event_matches_filter(const char *event_name, const char *filter_csv);

/* Opens a WebSocket connection to ws://<host>:<port><path>, subscribes
 * to events (the gateway broadcasts all events to every connection by
 * default — no client-side subscribe message needed), prints each
 * matching event to stdout + JSONL log, reconnects on disconnect up to
 * cfg->max_reconnect_attempts, returns HU_OK on clean exit (Ctrl+C) or
 * HU_ERR_IO on reconnect-failed.
 *
 * Not implemented yet — T2-T6. T1 ships the pure helpers above and the
 * stub returns HU_ERR_NOT_SUPPORTED.
 */
hu_error_t hu_doctor_ws_watch(hu_allocator_t *alloc, const hu_doctor_ws_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* HU_DOCTOR_WS_CONSUMER_H */
