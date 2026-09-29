#ifndef HU_RELIABLE_H
#define HU_RELIABLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Provider entry for multi-provider fallback chain */
typedef struct hu_reliable_provider_entry {
    const char *name;
    size_t name_len;
    hu_provider_t provider;
} hu_reliable_provider_entry_t;

/* Model + fallback models for per-model failover */
typedef struct hu_reliable_fallback_model {
    const char *model;
    size_t model_len;
} hu_reliable_fallback_model_t;

typedef struct hu_reliable_model_fallback_entry {
    const char *model;
    size_t model_len;
    const hu_reliable_fallback_model_t *fallbacks;
    size_t fallbacks_count;
} hu_reliable_model_fallback_entry_t;

typedef struct hu_reliable_config {
    hu_provider_t primary;
    hu_provider_t fallback;       /* optional, zeroed if none */
    int max_retries;              /* default 3 */
    int base_delay_ms;            /* default 1000 */
    int max_delay_ms;             /* default 30000 */
    int failure_threshold;        /* default 5 */
    int recovery_timeout_seconds; /* default 60 */
} hu_reliable_config_t;

/* Create a reliable provider from config (retry, fallback, circuit breaker). */
hu_error_t hu_reliable_provider_create(hu_allocator_t *alloc, const hu_reliable_config_t *config,
                                       hu_provider_t *out);

/* Create a reliable provider that wraps an inner provider with retry and exponential backoff.
 * max_retries: number of retries (0 = no retries, 1 = 2 total attempts)
 * backoff_ms: initial backoff in ms (min 50), doubles each retry up to 10000ms.
 * extras: optional fallback providers (NULL = none). Caller owns the array and providers.
 * model_fallbacks: optional per-model fallback chains (NULL = none). Caller owns the array.
 * In HU_IS_TEST: skips sleep, retries immediately.
 * Multi-provider: tries inner, then each extra in order for each model in the chain.
 * Model fallback: for model X, if configured, tries [X, fallback1, fallback2, ...] */
hu_error_t hu_reliable_create(hu_allocator_t *alloc, hu_provider_t inner, uint32_t max_retries,
                              uint64_t backoff_ms, hu_provider_t *out);

hu_error_t hu_reliable_create_ex(hu_allocator_t *alloc, hu_provider_t inner, uint32_t max_retries,
                                 uint64_t backoff_ms, const hu_reliable_provider_entry_t *extras,
                                 size_t extras_count,
                                 const hu_reliable_model_fallback_entry_t *model_fallbacks,
                                 size_t model_fallbacks_count, hu_provider_t *out);

/* ── Circuit breaker on the primary (2026-09-03) ────────────────────────
 *
 * Incident: mlx-server on :8741 died but its listening socket stayed open (a
 * ?E zombie). Every daemon request connected fine, then hung until the 300 s
 * low-speed timeout, was retried twice more at 300 s each, and only then fell
 * to the cloud provider — ~15 minutes per turn, for hours. Two rules fix it:
 *   1. a timed-out provider is NOT retried within the same call (each retry
 *      costs the full timeout window); the chain moves to the next provider;
 *   2. after `failure_threshold` consecutive primary failures the primary is
 *      skipped for `recovery_seconds` (circuit open); then ONE trial request
 *      is allowed (half-open) and a success closes the circuit.
 * hu_reliable_create_ex enables the circuit with the defaults below; callers
 * may override. hu_reliable_provider_create keeps its own 5 / 60 defaults. */
#define HU_RELIABLE_CIRCUIT_DEFAULT_THRESHOLD     2
#define HU_RELIABLE_CIRCUIT_DEFAULT_RECOVERY_SECS 300

/* Pure: does this error end the attempts on the CURRENT provider? True for
 * HU_ERR_TIMEOUT — retrying it re-pays the timeout window with the same server. */
bool hu_reliable_error_ends_provider_attempts(hu_error_t err);

/* Override the circuit parameters on a reliable provider. 0 keeps the current
 * value; a negative failure_threshold disables the circuit entirely. */
void hu_reliable_set_circuit(hu_provider_t *reliable, int failure_threshold, int recovery_seconds);

/* Observability (tests, doctor): consecutive primary failures and the time the
 * circuit stays open until (0 when closed). */
void hu_reliable_circuit_state(const hu_provider_t *reliable, int *out_failures,
                               time_t *out_open_until);

/* Test seam: inject the clock the circuit reads (NULL restores time(NULL)). */
void hu_reliable_set_clock(hu_provider_t *reliable, time_t (*now_fn)(void *), void *now_ud);

/* ── Empty-reply failover (2026-09-04) ───────────────────────────────────
 * A provider that returns HU_OK with no content has not answered. The local
 * model does this on certain prompts (think-only output); the daemon logged
 * "empty assistant response" and sent nothing, because its own cloud fallback
 * only fired on a model-router local model that production never configured.
 * ON by default: the empty reply is treated as HU_ERR_PROVIDER_RESPONSE for
 * that provider (no retry on it, no circuit-breaker credit) and the chain
 * moves to the mapped fallback model / next provider. */
void hu_reliable_set_empty_failover(hu_provider_t *reliable, bool on);

#endif /* HU_RELIABLE_H */
