/* Spec 2026-05-19 self-model-scaffold — Phase A (DOMAIN module).
 *
 * Per-turn behavioral observation ring buffer. See
 * include/human/agent/self_model.h for the contract.
 *
 * Privacy invariant (AC-SM-7): this TU stores hashes, sizes, enum
 * codes, and timestamps only. It MUST NEVER ingest user message
 * content, agent response bodies, tool arguments, or any other
 * content-carrying string fields. Pinned by the grep test in
 * tests/test_self_model_no_content_capture.c (Phase B).
 *
 * This module is the pure in-memory behavior log. The Phase-C persistence
 * (SQLite aggregation, drift detection, the agent_self_observations /
 * agent_self_concerns tables + the raw sqlite3 handle) was relocated to
 * src/memory/repos/self_model_repo_sqlite.c so this module no longer includes
 * <sqlite3.h> (memory repository pattern; sqlite-includer ratchet).
 *
 * Build gating per `.claude/rules/test-source-gate-symmetry.md`:
 * source body is wrapped in `#ifdef HU_ENABLE_SELF_MODEL`; the `#else`
 * branch provides bare-return stubs so the public symbols always
 * resolve at link time and callers do not need conditional
 * compilation. AC-SM-6 (zero-cost when OFF) is satisfied because the
 * stubs compile to a handful of bytes per function.
 */

#include "human/agent/self_model.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SELF_MODEL

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int self_model_log_is_initialized(const hu_agent_behavior_log_t *log) {
    return log != NULL && log->records != NULL && log->capacity > 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

hu_error_t hu_agent_behavior_log_init(hu_agent_behavior_log_t *log, const hu_allocator_t *allocator,
                                      size_t capacity) {
    if (log == NULL || allocator == NULL || allocator->alloc == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    /* Always zero the struct first so partial-init paths leave a safe
     * resting state (records=NULL, capacity=0, head=0). */
    memset(log, 0, sizeof(*log));

    if (capacity == 0) {
        capacity = HU_AGENT_BEHAVIOR_LOG_DEFAULT_CAPACITY;
    }
    if (capacity > HU_AGENT_BEHAVIOR_LOG_MAX_CAPACITY) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t slab_bytes = capacity * sizeof(hu_agent_behavior_record_t);
    hu_agent_behavior_record_t *slab =
        (hu_agent_behavior_record_t *)allocator->alloc(allocator->ctx, slab_bytes);
    if (slab == NULL) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(slab, 0, slab_bytes);

    log->records = slab;
    log->capacity = capacity;
    log->head = 0;
    log->allocator = *allocator;
    return HU_OK;
}

void hu_agent_behavior_log_destroy(hu_agent_behavior_log_t *log) {
    if (log == NULL) {
        return;
    }
    if (log->records != NULL && log->allocator.free != NULL) {
        size_t slab_bytes = log->capacity * sizeof(hu_agent_behavior_record_t);
        log->allocator.free(log->allocator.ctx, log->records, slab_bytes);
    }
    memset(log, 0, sizeof(*log));
}

hu_error_t hu_agent_behavior_log_record(hu_agent_behavior_log_t *log,
                                        const hu_agent_behavior_record_t *rec) {
    /* Silently tolerant of NULL / uninitialized log. The hot path must
     * not punish callers for a misconfigured subsystem; the operator-
     * visible signal lives in the one-shot init log line (Phase B). */
    if (!self_model_log_is_initialized(log) || rec == NULL) {
        return HU_OK;
    }
    /* Zero-alloc fast path: value-copy into slot, increment monotonic
     * head. Wrap is implicit via head % capacity at snapshot time. */
    size_t slot = log->head % log->capacity;
    log->records[slot] = *rec;
    log->head++;
    return HU_OK;
}

hu_error_t hu_agent_behavior_log_snapshot(const hu_agent_behavior_log_t *log,
                                          hu_agent_behavior_record_t *out, size_t max_out,
                                          size_t *out_count) {
    if (out_count == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;
    if (!self_model_log_is_initialized(log) || out == NULL || max_out == 0) {
        return HU_OK;
    }

    /* Number of records actually present in the buffer. Either head
     * (if we have not yet wrapped) or capacity (if we have). */
    size_t available = (log->head < log->capacity) ? log->head : log->capacity;
    size_t n = (max_out < available) ? max_out : available;
    if (n == 0) {
        return HU_OK;
    }

    if (log->head <= log->capacity) {
        /* No wrap yet — slots [0 .. head) hold the records in order.
         * Copy the last n of those. */
        size_t start = log->head - n;
        memcpy(out, &log->records[start], n * sizeof(hu_agent_behavior_record_t));
    } else {
        /* Wrapped. The physically-oldest record sits at slot
         * (head % capacity); records are chronological from there for
         * `capacity` slots. We want the most recent n of those, in
         * chronological order. Walk forward from
         * ((head - n) % capacity) for n records. */
        size_t start = (log->head - n) % log->capacity;
        if (start + n <= log->capacity) {
            memcpy(out, &log->records[start], n * sizeof(hu_agent_behavior_record_t));
        } else {
            size_t first = log->capacity - start;
            memcpy(out, &log->records[start], first * sizeof(hu_agent_behavior_record_t));
            memcpy(&out[first], &log->records[0], (n - first) * sizeof(hu_agent_behavior_record_t));
        }
    }
    *out_count = n;
    return HU_OK;
}

size_t hu_agent_behavior_log_total_records(const hu_agent_behavior_log_t *log) {
    if (log == NULL) {
        return 0;
    }
    return log->head;
}

hu_error_t hu_agent_self_model_build_directive(const hu_agent_behavior_log_t *log,
                                               const hu_allocator_t *alloc, char **out,
                                               size_t *out_len) {
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!log || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    /* Aggregate the recent window. Privacy invariant holds: we read only
     * sizes + the emotion enum shadow — never any content. */
    hu_agent_behavior_record_t recs[16];
    size_t n = 0;
    if (hu_agent_behavior_log_snapshot(log, recs, 16, &n) != HU_OK || n < 3)
        return HU_OK; /* too few turns to say anything meaningful — no directive */

    uint64_t total_chars = 0;
    size_t emo_counts[5] = {0, 0, 0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        total_chars += recs[i].response_length_chars;
        uint8_t e = recs[i].emotional_register;
        if (e < 5)
            emo_counts[e]++;
    }
    unsigned mean_chars = (unsigned)(total_chars / (uint64_t)n);

    size_t dom = 0;
    for (size_t i = 1; i < 5; i++) {
        if (emo_counts[i] > emo_counts[dom])
            dom = i;
    }
    /* Index by hu_agent_emotional_register_t: NEUTRAL, POSITIVE, NEGATIVE,
     * CAUTIOUS, OTHER. */
    static const char *const tone[5] = {"pretty even", "warm and positive", "a bit more guarded",
                                        "careful and measured", "all over the place"};

    char buf[320];
    int w = snprintf(buf, sizeof(buf),
                     "[self-awareness] Over your last %zu replies you've averaged about %u "
                     "characters and your tone has mostly read as %s. Keep showing up the way "
                     "you naturally do — stay consistent rather than over-correcting.",
                     n, mean_chars, tone[dom]);
    if (w <= 0 || (size_t)w >= sizeof(buf))
        return HU_OK;

    char *d = (char *)alloc->alloc(alloc->ctx, (size_t)w + 1);
    if (!d)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(d, buf, (size_t)w);
    d[w] = '\0';
    *out = d;
    *out_len = (size_t)w;
    return HU_OK;
}

#else /* !HU_ENABLE_SELF_MODEL */

/* OFF-variant stubs. Match `.claude/rules/test-source-gate-symmetry.md`'s
 * "internal #ifdef wrap with stub runner" pattern: keep the symbols
 * resolvable at link time so callers do not need conditional compilation.
 * Bare returns; no allocation, no I/O.
 *
 * `(void)` casts suppress -Wunused-parameter without restating the
 * struct shapes. */

hu_error_t hu_agent_behavior_log_init(hu_agent_behavior_log_t *log, const hu_allocator_t *allocator,
                                      size_t capacity) {
    (void)allocator;
    (void)capacity;
    if (log != NULL) {
        memset(log, 0, sizeof(*log));
    }
    return HU_OK;
}

void hu_agent_behavior_log_destroy(hu_agent_behavior_log_t *log) {
    if (log != NULL) {
        memset(log, 0, sizeof(*log));
    }
}

hu_error_t hu_agent_behavior_log_record(hu_agent_behavior_log_t *log,
                                        const hu_agent_behavior_record_t *rec) {
    (void)log;
    (void)rec;
    return HU_OK;
}

hu_error_t hu_agent_behavior_log_snapshot(const hu_agent_behavior_log_t *log,
                                          hu_agent_behavior_record_t *out, size_t max_out,
                                          size_t *out_count) {
    (void)log;
    (void)out;
    (void)max_out;
    if (out_count != NULL) {
        *out_count = 0;
    }
    return HU_OK;
}

size_t hu_agent_behavior_log_total_records(const hu_agent_behavior_log_t *log) {
    (void)log;
    return 0;
}

hu_error_t hu_agent_self_model_build_directive(const hu_agent_behavior_log_t *log,
                                               const hu_allocator_t *alloc, char **out,
                                               size_t *out_len) {
    (void)log;
    (void)alloc;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_OK; /* self-model OFF → no self-observation directive */
}

#endif /* HU_ENABLE_SELF_MODEL */
