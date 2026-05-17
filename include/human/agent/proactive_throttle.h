/*
 * proactive_throttle.h — Centralized throttle predicates for proactive sends.
 *
 * Background: prior to 2026-05-16, F25/F30/F31 proactive messages bypassed the
 * channel rate limiter (only inbound traffic was metered) and used fixed [8]
 * ring buffers for per-contact dedup. The 2026-05-16 incident showed both
 * failure modes in production: "how'd it go with the loan?" fired 4 times
 * back-to-back to Mindy (no rate-limit gate on the proactive send path), and
 * with >8 contacts the daily message dedup wrapped silently.
 *
 * This module factors the throttle decisions into pure, testable predicates
 * per `.claude/rules/security-predicate-extraction.md`. The decisions are:
 *
 *  - per-channel rate-limiter (P1-6 — was only on inbound)
 *  - per-(contact_id, ymd) daily dedup (P1-7 — replaces fixed [8] ring buffer)
 *  - per-contact daily/weekly send cap (P4-6 — new defense-in-depth)
 *
 * Storage is a flat heap-backed table sized at create time. Tables are kept
 * small (each entry <128 bytes) and grow with allocator->alloc; eviction is
 * LRU when capacity is exhausted. ymd dedup self-resets on date rollover.
 */
#ifndef HU_AGENT_PROACTIVE_THROTTLE_H
#define HU_AGENT_PROACTIVE_THROTTLE_H

#include "human/channels/rate_limit.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_PROACTIVE_THROTTLE_MAX_CHANNELS 32
#define HU_PROACTIVE_THROTTLE_MAX_CONTACTS 256
#define HU_PROACTIVE_THROTTLE_MAX_SENDS    1024

/* Per-channel rate limiter slot. */
typedef struct hu_proactive_throttle_channel {
    char name[32];
    hu_channel_rate_limiter_t lim;
    uint64_t last_used_ms; /* for LRU eviction */
} hu_proactive_throttle_channel_t;

/* Per-(contact_id, ymd) one-shot dedup slot. */
typedef struct hu_proactive_throttle_dedup {
    char key[96]; /* "<feature>:<contact_id>:<ymd>" */
    uint32_t ymd; /* YYYYMMDD; used for cheap rollover */
    uint64_t last_used_ms;
} hu_proactive_throttle_dedup_t;

/* Per-send audit log entry (used for daily/weekly cap counting). */
typedef struct hu_proactive_throttle_send {
    char contact_id[64];
    char feature[16]; /* "F25", "F30", "F31", "proactive", ... */
    uint64_t sent_at_ms;
} hu_proactive_throttle_send_t;

typedef struct hu_proactive_throttle {
    hu_allocator_t *alloc;
    hu_proactive_throttle_channel_t channels[HU_PROACTIVE_THROTTLE_MAX_CHANNELS];
    size_t channel_count;
    hu_proactive_throttle_dedup_t dedup[HU_PROACTIVE_THROTTLE_MAX_CONTACTS];
    size_t dedup_count;
    /* Send log is a ring buffer — order matters but we only ever scan recent. */
    hu_proactive_throttle_send_t sends[HU_PROACTIVE_THROTTLE_MAX_SENDS];
    size_t send_head;  /* next write slot */
    size_t send_count; /* up to HU_PROACTIVE_THROTTLE_MAX_SENDS */
    /* Configurable caps; default 1/24h, 3/7d per contact. */
    uint32_t daily_cap;
    uint32_t weekly_cap;
} hu_proactive_throttle_t;

/* Initialize the throttle structure in place. Safe to call on a stack object. */
void hu_proactive_throttle_init(hu_proactive_throttle_t *t, hu_allocator_t *alloc);

/* Try to consume a token from the per-channel rate limiter. Lazy-allocates a
 * default limiter for first use of each channel name. Returns true if allowed.
 * channel_name NULL/empty is treated as "no limiter applies" and returns true. */
bool hu_proactive_throttle_channel_try_consume(hu_proactive_throttle_t *t,
                                               const char *channel_name);

/* Returns true if (feature, contact_id) has NOT been delivered on the given
 * ymd yet, AND atomically marks it so this call wins. ymd is computed by the
 * caller (YYYYMMDD) to make rollover deterministic in tests. */
bool hu_proactive_throttle_dedup_first_today(hu_proactive_throttle_t *t, const char *feature,
                                             const char *contact_id, uint32_t ymd);

/* Record a send and return false if the per-contact daily (24h) or weekly (7d)
 * cap would be exceeded. The caller is expected to call this AFTER the channel
 * rate-limiter has accepted the send but BEFORE actually transmitting; if the
 * function returns false, the caller MUST skip the send. The recorded entry
 * still lands on success so subsequent calls see it. */
bool hu_proactive_throttle_record_send(hu_proactive_throttle_t *t, const char *contact_id,
                                       const char *feature, uint64_t now_ms);

/* Count sends to a contact within the given trailing window (used by tests). */
uint32_t hu_proactive_throttle_count_in_window(const hu_proactive_throttle_t *t,
                                               const char *contact_id, uint64_t now_ms,
                                               uint64_t window_ms);

/* Reset the entire throttle state — tests only. */
void hu_proactive_throttle_reset(hu_proactive_throttle_t *t);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_PROACTIVE_THROTTLE_H */
