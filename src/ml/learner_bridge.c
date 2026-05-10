/* W13 — Learner signal bridge (see include/human/ml/learner_bridge.h).
 *
 * Two thin emitters convert real signal sources into pending training
 * signals on the learner. Both are designed to be called frequently with
 * overlapping inputs (e.g. the outcome tracker's whole circular buffer
 * scanned once per minute) — the watermarks guarantee that replays
 * produce signals exactly once.
 *
 * Determinism: the bridge does no I/O, no clock reads, and no random
 * number generation. Its only outputs are deterministic transforms of
 * its inputs and the pre-existing watermark state on the learner. */

#include "human/ml/learner_bridge.h"

#include <stdint.h>
#include <string.h>

/* Grow the pending buffer to hold at least `needed` entries, capped at
 * HU_LEARNER_PENDING_MAX. Returns HU_OK on success, HU_ERR_OUT_OF_MEMORY
 * if allocation fails. The buffer is shrink-stable: we only grow. */
static hu_error_t ensure_pending_capacity(hu_learner_t *l, size_t needed) {
    if (needed > HU_LEARNER_PENDING_MAX)
        needed = HU_LEARNER_PENDING_MAX;
    if (l->pending_cap >= needed)
        return HU_OK;

    size_t new_cap = l->pending_cap == 0 ? 16 : l->pending_cap * 2;
    if (new_cap < needed)
        new_cap = needed;
    if (new_cap > HU_LEARNER_PENDING_MAX)
        new_cap = HU_LEARNER_PENDING_MAX;

    hu_training_signal_t *grown = NULL;
    if (l->pending) {
        grown = (hu_training_signal_t *)l->alloc->realloc(
            l->alloc->ctx, l->pending, l->pending_cap * sizeof(*grown), new_cap * sizeof(*grown));
    } else {
        grown = (hu_training_signal_t *)l->alloc->alloc(l->alloc->ctx, new_cap * sizeof(*grown));
    }
    if (!grown)
        return HU_ERR_OUT_OF_MEMORY;
    /* Zero the newly-grown tail so any unset union members read as 0. */
    memset(grown + l->pending_cap, 0, (new_cap - l->pending_cap) * sizeof(*grown));
    l->pending = grown;
    l->pending_cap = new_cap;
    return HU_OK;
}

/* Push one signal onto the pending buffer. Returns true on success,
 * false when the cap is reached (signal silently dropped — watermark
 * advancement is the caller's responsibility). */
static bool push_pending(hu_learner_t *l, const hu_training_signal_t *s) {
    if (l->pending_count >= HU_LEARNER_PENDING_MAX)
        return false;
    if (ensure_pending_capacity(l, l->pending_count + 1) != HU_OK)
        return false;
    l->pending[l->pending_count++] = *s;
    return true;
}

hu_error_t hu_learner_bridge_emit_persona_deltas(hu_learner_t *learner,
                                                 const hu_persona_delta_t *deltas, size_t n) {
    /* NULL learner is intentional no-op so callers (delta_observer.c) can
     * pass through unconditionally. */
    if (!learner || !learner->alloc)
        return HU_OK;
    if (n == 0)
        return HU_OK;
    if (!deltas)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t high = learner->pending_persona_delta_id_high;
    int64_t new_high = high;

    for (size_t i = 0; i < n; i++) {
        const hu_persona_delta_t *d = &deltas[i];
        /* id == 0 means "the caller didn't get an id back from the propose
         * layer" — we still want those to flow through as signals, but we
         * can't dedupe them on id alone. Fall through to push and let the
         * watermark catch any later, properly-ided replay. */
        if (d->id != 0 && d->id <= high)
            continue;

        hu_training_signal_t s;
        memset(&s, 0, sizeof(s));
        s.kind = HU_TRAIN_PERSONA_DELTA;
        s.as.persona.delta = *d;
        s.observed_at = d->proposed_at_ms;
        push_pending(learner, &s);

        if (d->id > new_high)
            new_high = d->id;
    }

    learner->pending_persona_delta_id_high = new_high;
    return HU_OK;
}

/* Map an outcome type to a (kind, reward) pair for HU_TRAIN_CASE_OUTCOME.
 * All outcomes are emitted as case_outcome signals; the reward is what
 * encodes positive vs negative feedback. */
static float reward_for_outcome(hu_outcome_type_t t) {
    switch (t) {
    case HU_OUTCOME_TOOL_SUCCESS:
    case HU_OUTCOME_USER_POSITIVE:
        return 1.0f;
    case HU_OUTCOME_TOOL_FAILURE:
    case HU_OUTCOME_USER_CORRECTION:
        return 0.0f;
    }
    /* Defensive: any future enum value defaults to neutral. */
    return 0.5f;
}

hu_error_t hu_learner_bridge_emit_outcomes(hu_learner_t *learner, hu_outcome_tracker_t *tracker) {
    if (!learner || !learner->alloc || !tracker)
        return HU_OK;

    size_t count = 0;
    const hu_outcome_entry_t *entries = hu_outcome_get_recent(tracker, &count);
    if (!entries || count == 0)
        return HU_OK;

    int64_t high_ms = learner->pending_outcome_ts_high;
    int64_t new_high = high_ms;

    /* hu_outcome_get_recent returns the underlying ring buffer pointer, so
     * we walk all `count` slots and filter by timestamp. Slots that have
     * never been written read as zero (timestamp_ms == 0) and are skipped
     * automatically by the > 0 comparison. */
    for (size_t i = 0; i < count; i++) {
        const hu_outcome_entry_t *e = &entries[i];
        int64_t ts = (int64_t)e->timestamp_ms;
        if (ts <= 0)
            continue;
        if (ts <= high_ms)
            continue;

        hu_training_signal_t s;
        memset(&s, 0, sizeof(s));
        s.kind = HU_TRAIN_CASE_OUTCOME;
        /* case_id derived from timestamp: deterministic, non-zero, allows
         * downstream learners to dedupe within a training run. */
        s.as.case_outcome.case_id = ts;
        s.as.case_outcome.reward = reward_for_outcome(e->type);
        s.observed_at = ts;
        push_pending(learner, &s);

        if (ts > new_high)
            new_high = ts;
    }

    learner->pending_outcome_ts_high = new_high;
    return HU_OK;
}

hu_error_t hu_learner_pending_drain(hu_learner_t *learner, hu_training_signal_t **out,
                                    size_t *out_count) {
    if (!learner || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (learner->pending_count == 0)
        return HU_OK;

    size_t n = learner->pending_count;
    hu_training_signal_t *copy =
        (hu_training_signal_t *)learner->alloc->alloc(learner->alloc->ctx, n * sizeof(*copy));
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, learner->pending, n * sizeof(*copy));
    /* Reset the buffer (but keep the allocation around for the next emit
     * burst — saves alloc thrashing in the common case). */
    learner->pending_count = 0;
    *out = copy;
    *out_count = n;
    return HU_OK;
}

size_t hu_learner_pending_count(const hu_learner_t *learner) {
    return learner ? learner->pending_count : 0;
}
