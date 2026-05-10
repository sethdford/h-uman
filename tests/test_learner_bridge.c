/* W13 — Learner signal bridge tests.
 *
 * These tests cover the thin glue between two real signal sources
 * (persona delta proposals and outcome tracker entries) and the
 * `hu_learner_t` pending-signal buffer. They never invoke the learner's
 * vtable train(), so they don't write any adapter files; they verify
 * pure signal-emit behavior:
 *
 *   - n=0 / NULL inputs are silent no-ops
 *   - real inputs produce the expected pending signal kind
 *   - replays are idempotent (watermark advancement)
 *   - outcome tracker entries flow through with the right reward
 *
 * The mock learner backend below is the smallest possible valid
 * `hu_learner_vtable_t`: open() succeeds, train() always fails (so any
 * accidental call shows up in test output), deinit() is trivial. We do
 * NOT use the production CPU backend here because we explicitly don't
 * want any file I/O on the test host. */

#include "human/agent/outcomes.h"
#include "human/core/allocator.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* ── Mock learner ──────────────────────────────────────────────────────
 *
 * A skeleton learner that satisfies the bridge's only requirement:
 * a non-NULL `alloc` pointer. The bridge never invokes vt->train, so the
 * vtable can be a stub. We allocate the struct ourselves (instead of
 * going through hu_learner_open_default) so the test never depends on
 * which backends compiled in. */

static hu_learner_t *mock_learner_open(hu_allocator_t *alloc) {
    hu_learner_t *l = (hu_learner_t *)alloc->alloc(alloc->ctx, sizeof(*l));
    if (!l)
        return NULL;
    memset(l, 0, sizeof(*l));
    l->alloc = alloc;
    /* vt and ctx left NULL — the bridge never touches them. */
    return l;
}

static void mock_learner_close(hu_learner_t *l) {
    if (!l)
        return;
    if (l->pending && l->alloc)
        l->alloc->free(l->alloc->ctx, l->pending, l->pending_cap * sizeof(*l->pending));
    if (l->alloc)
        l->alloc->free(l->alloc->ctx, l, sizeof(*l));
}

/* Make a synthetic persona delta with the requested id. Used by the
 * persona-delta tests to drive the bridge directly without touching a
 * graph or sqlite. */
static hu_persona_delta_t make_delta(int64_t id, hu_persona_delta_kind_t kind, const char *value,
                                     int64_t proposed_at_ms) {
    hu_persona_delta_t d;
    memset(&d, 0, sizeof(d));
    d.id = id;
    d.kind = kind;
    d.confidence = 0.8f;
    d.proposed_at_ms = proposed_at_ms;
    d.status = HU_DELTA_STATUS_PENDING;
    if (value)
        snprintf(d.value, sizeof(d.value), "%s", value);
    snprintf(d.source, sizeof(d.source), "test");
    return d;
}

/* ── Bridge — persona deltas ──────────────────────────────────────── */

static void bridge_from_deltas_n0_no_op(void) {
    hu_learner_t *l = mock_learner_open(A());
    HU_ASSERT_NOT_NULL(l);

    /* n == 0: no signals, no allocation, no watermark change. */
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(l, NULL, 0), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 0);
    HU_ASSERT_EQ((int)l->pending_persona_delta_id_high, 0);

    /* NULL learner: also silent no-op. */
    hu_persona_delta_t d = make_delta(1, HU_PERSONA_DELTA_TONE, "warmer", 1000);
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(NULL, &d, 1), HU_OK);

    mock_learner_close(l);
}

static void bridge_from_deltas_proposes_signal(void) {
    hu_learner_t *l = mock_learner_open(A());
    HU_ASSERT_NOT_NULL(l);

    hu_persona_delta_t deltas[3];
    deltas[0] = make_delta(1, HU_PERSONA_DELTA_TONE, "warmer", 1000);
    deltas[1] = make_delta(2, HU_PERSONA_DELTA_LENGTH, "shorter", 1100);
    deltas[2] = make_delta(3, HU_PERSONA_DELTA_VOCAB_AVOID, "literally", 1200);

    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(l, deltas, 3), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 3);
    HU_ASSERT_EQ((int)l->pending_persona_delta_id_high, 3);

    /* Each pending entry must be a HU_TRAIN_PERSONA_DELTA carrying the
     * source delta verbatim (kind + value + observed_at). */
    HU_ASSERT_EQ((int)l->pending[0].kind, HU_TRAIN_PERSONA_DELTA);
    HU_ASSERT_STR_EQ(l->pending[0].as.persona.delta.value, "warmer");
    HU_ASSERT_EQ(l->pending[0].observed_at, 1000);
    HU_ASSERT_EQ((int)l->pending[1].kind, HU_TRAIN_PERSONA_DELTA);
    HU_ASSERT_STR_EQ(l->pending[1].as.persona.delta.value, "shorter");
    HU_ASSERT_EQ((int)l->pending[2].kind, HU_TRAIN_PERSONA_DELTA);
    HU_ASSERT_STR_EQ(l->pending[2].as.persona.delta.value, "literally");

    /* Drain transfers ownership and resets the buffer; watermark stays. */
    hu_training_signal_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_learner_pending_drain(l, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 3);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 0);
    HU_ASSERT_EQ((int)l->pending_persona_delta_id_high, 3);
    hu_learner_signals_free(A(), out, n);

    mock_learner_close(l);
}

static void bridge_from_deltas_idempotent_on_replay(void) {
    hu_learner_t *l = mock_learner_open(A());
    HU_ASSERT_NOT_NULL(l);

    hu_persona_delta_t deltas[2];
    deltas[0] = make_delta(10, HU_PERSONA_DELTA_TONE, "warmer", 1000);
    deltas[1] = make_delta(11, HU_PERSONA_DELTA_LENGTH, "shorter", 1100);

    /* First emit: 2 signals queued. */
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(l, deltas, 2), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 2);
    HU_ASSERT_EQ((int)l->pending_persona_delta_id_high, 11);

    /* Replay with the same deltas: watermark filters them out. Pending
     * count stays at 2 (no double-emit). */
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(l, deltas, 2), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 2);
    HU_ASSERT_EQ((int)l->pending_persona_delta_id_high, 11);

    /* Mixed batch with one already-seen + one new: only the new one
     * emits, watermark advances only as far as the new one. */
    hu_persona_delta_t mixed[2];
    mixed[0] = deltas[1];                                                        /* id=11, seen */
    mixed[1] = make_delta(12, HU_PERSONA_DELTA_VOCAB_AVOID, "actually", 1200);   /* new */
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(l, mixed, 2), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 3);
    HU_ASSERT_EQ((int)l->pending_persona_delta_id_high, 12);

    mock_learner_close(l);
}

/* ── Bridge — outcomes ────────────────────────────────────────────── */

static void bridge_from_outcomes_drains_recent(void) {
    hu_learner_t *l = mock_learner_open(A());
    HU_ASSERT_NOT_NULL(l);

    hu_outcome_tracker_t t;
    hu_outcome_tracker_init(&t, false);

    /* Empty tracker: silent no-op, nothing queued. */
    HU_ASSERT_EQ(hu_learner_bridge_emit_outcomes(l, &t), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 0);

    /* Record four outcomes: success, failure, positive, correction. The
     * tracker stamps each with its internal clock (HU_IS_TEST returns a
     * fixed value), so they share a timestamp — but the bridge filters
     * by `> watermark`, not `!= watermark`, so all four still flow
     * through on the first emit. */
    hu_outcome_record_tool(&t, "calendar", true, "scheduled");
    hu_outcome_record_tool(&t, "calendar", false, "no permission");
    hu_outcome_record_positive(&t, "thanks");
    hu_outcome_record_correction(&t, NULL, "be more concise");

    HU_ASSERT_EQ(hu_learner_bridge_emit_outcomes(l, &t), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 4);
    HU_ASSERT(l->pending_outcome_ts_high > 0);

    /* All four signals are HU_TRAIN_CASE_OUTCOME with a deterministic
     * case_id (the timestamp). Verify rewards are mapped correctly. */
    int saw_success = 0, saw_failure = 0, saw_positive = 0, saw_correction = 0;
    for (size_t i = 0; i < 4; i++) {
        HU_ASSERT_EQ((int)l->pending[i].kind, HU_TRAIN_CASE_OUTCOME);
        HU_ASSERT(l->pending[i].as.case_outcome.case_id > 0);
        float r = l->pending[i].as.case_outcome.reward;
        if (r >= 0.99f && !saw_success)
            saw_success = 1;
        else if (r >= 0.99f && !saw_positive)
            saw_positive = 1;
        else if (r <= 0.01f && !saw_failure)
            saw_failure = 1;
        else if (r <= 0.01f && !saw_correction)
            saw_correction = 1;
    }
    HU_ASSERT(saw_success && saw_failure && saw_positive && saw_correction);

    /* Replay with the same tracker: watermark filters everything. */
    HU_ASSERT_EQ(hu_learner_bridge_emit_outcomes(l, &t), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 4);

    mock_learner_close(l);
}

static void bridge_from_outcomes_null_safe(void) {
    hu_learner_t *l = mock_learner_open(A());
    HU_ASSERT_NOT_NULL(l);

    /* NULL tracker: returns OK, no allocation, no signal. */
    HU_ASSERT_EQ(hu_learner_bridge_emit_outcomes(l, NULL), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(l), 0);

    /* NULL learner: returns OK with any (or NULL) tracker. */
    hu_outcome_tracker_t t;
    hu_outcome_tracker_init(&t, false);
    hu_outcome_record_tool(&t, "calendar", true, "scheduled");
    HU_ASSERT_EQ(hu_learner_bridge_emit_outcomes(NULL, &t), HU_OK);
    HU_ASSERT_EQ(hu_learner_bridge_emit_outcomes(NULL, NULL), HU_OK);

    /* Drain on a NULL learner is rejected. */
    hu_training_signal_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_learner_pending_drain(NULL, &out, &n), HU_ERR_INVALID_ARGUMENT);

    /* Drain on an empty learner returns OK with nothing. */
    HU_ASSERT_EQ(hu_learner_pending_drain(l, &out, &n), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((int)n, 0);

    /* hu_learner_pending_count(NULL) returns 0. */
    HU_ASSERT_EQ((int)hu_learner_pending_count(NULL), 0);

    mock_learner_close(l);
}

/* ── Test runner ──────────────────────────────────────────────────── */

void run_learner_bridge_tests(void) {
    HU_TEST_SUITE("learner_bridge - signal collection from persona deltas + outcomes");

    HU_RUN_TEST(bridge_from_deltas_n0_no_op);
    HU_RUN_TEST(bridge_from_deltas_proposes_signal);
    HU_RUN_TEST(bridge_from_deltas_idempotent_on_replay);
    HU_RUN_TEST(bridge_from_outcomes_drains_recent);
    HU_RUN_TEST(bridge_from_outcomes_null_safe);
}
