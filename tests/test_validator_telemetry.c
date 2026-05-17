/* test_validator_telemetry.c — Unit tests for hu_observer_emit_validator_decision.
 *
 * Uses a minimal capturing observer (in-file mock vtable) that stores the last
 * received event in a stack-allocated buffer so we can assert on its fields
 * without heap allocation.
 *
 * AC coverage:
 *   AC-5.1  enum tag + union arm (compile-time: test references the tag)
 *   AC-5.2  PASS suppression (no event emitted for PASS outcome)
 *   AC-5.3  REJECT path: decision=="reject", validator_name matches, bytes_stripped==0
 *   AC-5.4  REWRITE path: decision=="rewrite", bytes_stripped > 0
 *   AC-5.5  NULL observer: no crash, full code path executes
 */

#include "human/agent/output_validator.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/observability/validator_telemetry.h"
#include "human/observer.h"
#include "test_framework.h"
#include <string.h>

/* ── Capturing observer ──────────────────────────────────────────────────── */

typedef struct {
    int event_count;
    hu_observer_event_t last_event;
    /* Stable storage for string fields (the helper passes non-owning pointers;
     * we copy them here so assertions can run after the chain result is freed). */
    char decision_buf[32];
    char validator_name_buf[128];
    char channel_id_buf[64];
    char persona_name_buf[64];
} capture_ctx_t;

static void capture_record_event(void *ctx, const hu_observer_event_t *event) {
    capture_ctx_t *c = (capture_ctx_t *)ctx;
    c->event_count++;
    c->last_event = *event;
    /* Copy volatile string fields into stable buffers. */
    if (event->tag == HU_OBSERVER_EVENT_VALIDATOR_DECISION) {
        const char *dec = event->data.validator_decision.decision;
        const char *vn = event->data.validator_decision.validator_name;
        const char *ch = event->data.validator_decision.channel_id;
        const char *pn = event->data.validator_decision.persona_name;
        strncpy(c->decision_buf, dec ? dec : "", sizeof(c->decision_buf) - 1);
        c->decision_buf[sizeof(c->decision_buf) - 1] = '\0';
        strncpy(c->validator_name_buf, vn ? vn : "", sizeof(c->validator_name_buf) - 1);
        c->validator_name_buf[sizeof(c->validator_name_buf) - 1] = '\0';
        strncpy(c->channel_id_buf, ch ? ch : "", sizeof(c->channel_id_buf) - 1);
        c->channel_id_buf[sizeof(c->channel_id_buf) - 1] = '\0';
        strncpy(c->persona_name_buf, pn ? pn : "", sizeof(c->persona_name_buf) - 1);
        c->persona_name_buf[sizeof(c->persona_name_buf) - 1] = '\0';
        /* Redirect string pointers to stable buffers for later assertion. */
        c->last_event.data.validator_decision.decision = c->decision_buf;
        c->last_event.data.validator_decision.validator_name = c->validator_name_buf;
        c->last_event.data.validator_decision.channel_id = c->channel_id_buf;
        c->last_event.data.validator_decision.persona_name = c->persona_name_buf;
    }
}

static void capture_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    (void)ctx;
    (void)metric;
}

static void capture_flush(void *ctx) {
    (void)ctx;
}

static const char *capture_name(void *ctx) {
    (void)ctx;
    return "capture";
}

static const hu_observer_vtable_t capture_vtable = {
    .record_event = capture_record_event,
    .record_metric = capture_record_metric,
    .flush = capture_flush,
    .name = capture_name,
    .deinit = NULL,
};

static hu_observer_t make_capture_observer(capture_ctx_t *ctx) {
    hu_observer_t obs = {.ctx = ctx, .vtable = &capture_vtable};
    return obs;
}

/* ── Fixtures ────────────────────────────────────────────────────────────── */

/* F1 from tests/test_validators_persona_safety.c — contains persona narrator leak
 * that triggers HU_VALIDATOR_REJECT from hu_validator_persona_narrator. */
static const char *JORDAN_LEAK_F1 =
    "Wait, looking at the history, the AI has been slipping into "
    "\"How can I help you today?\" which is a massive AI tell and "
    "explicitly forbidden by the persona instructions. I need to snap "
    "back into Seth.\n\n"
    "Seth is chill, playful, and romantic with Jordan.\n"
    "If she says \"Oh nice!\", he should probably keep it light or ask a follow-up";

/* F2 contains assistant_closer suffix that triggers HU_VALIDATOR_REWRITE. */
static const char *ASSISTANT_CLOSER_F2 =
    "made my night tbh\n"
    "I'm all set, thank you! Is there anything I can help you with?";

/* Clean message — no violations. */
static const char *CLEAN_MESSAGE = "wait you got the package already? that was fast";

/* ── AC-5.3: REJECT path emits event with correct fields ─────────────────── */

static void telemetry_reject_emits_event_with_correct_decision(void) {
    hu_allocator_t alloc = hu_system_allocator();
    capture_ctx_t cap;
    memset(&cap, 0, sizeof(cap));
    hu_observer_t obs = make_capture_observer(&cap);

    /* Build the default outbound chain with persona "Seth" (enables persona_narrator). */
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain), HU_OK);

    hu_validator_context_t vctx = {0};
    vctx.persona_name = "Seth";
    vctx.persona_name_len = 4;
    vctx.channel_id = "imessage";
    vctx.channel_id_len = 8;

    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    size_t input_len = strlen(JORDAN_LEAK_F1);
    hu_error_t err =
        hu_output_validator_chain_execute(chain, &alloc, &vctx, JORDAN_LEAK_F1, input_len, &cr);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);

    /* AC-5.3 — emit and assert. */
    hu_observer_emit_validator_decision(&obs, &cr, &vctx, input_len);

    HU_ASSERT_EQ(cap.event_count, 1);
    HU_ASSERT_EQ(cap.last_event.tag, HU_OBSERVER_EVENT_VALIDATOR_DECISION); /* AC-5.1 */
    HU_ASSERT_STR_EQ(cap.decision_buf, "reject");
    /* validator_name must be non-empty (the persona_narrator validator name). */
    HU_ASSERT(cap.validator_name_buf[0] != '\0');
    HU_ASSERT_STR_EQ(cap.channel_id_buf, "imessage");
    HU_ASSERT_STR_EQ(cap.persona_name_buf, "Seth");
    HU_ASSERT_EQ(cap.last_event.data.validator_decision.response_len, input_len);
    HU_ASSERT_EQ(cap.last_event.data.validator_decision.bytes_stripped, (size_t)0);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* ── AC-5.4: REWRITE path emits event with bytes_stripped > 0 ───────────── */

static void telemetry_rewrite_emits_event_with_bytes_stripped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    capture_ctx_t cap;
    memset(&cap, 0, sizeof(cap));
    hu_observer_t obs = make_capture_observer(&cap);

    /* Build chain without persona name — assistant_closer fires as REWRITE. */
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, NULL, 0, &chain), HU_OK);

    hu_validator_context_t vctx = {0};
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    size_t input_len = strlen(ASSISTANT_CLOSER_F2);
    hu_error_t err = hu_output_validator_chain_execute(chain, &alloc, &vctx, ASSISTANT_CLOSER_F2,
                                                       input_len, &cr);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REWRITE);

    /* AC-5.4 — emit and assert. */
    hu_observer_emit_validator_decision(&obs, &cr, &vctx, input_len);

    HU_ASSERT_EQ(cap.event_count, 1);
    HU_ASSERT_STR_EQ(cap.decision_buf, "rewrite");
    HU_ASSERT_EQ(cap.last_event.data.validator_decision.response_len, input_len);
    /* bytes_stripped must be > 0 since text was shortened. */
    HU_ASSERT(cap.last_event.data.validator_decision.bytes_stripped > 0);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* ── AC-5.2: PASS suppression — no event emitted for clean message ────────── */

static void telemetry_pass_suppressed_no_event_emitted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    capture_ctx_t cap;
    memset(&cap, 0, sizeof(cap));
    hu_observer_t obs = make_capture_observer(&cap);

    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, NULL, 0, &chain), HU_OK);

    hu_validator_context_t vctx = {0};
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    size_t input_len = strlen(CLEAN_MESSAGE);
    hu_error_t err =
        hu_output_validator_chain_execute(chain, &alloc, &vctx, CLEAN_MESSAGE, input_len, &cr);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_PASS);

    /* AC-5.2 — PASS must be suppressed. */
    hu_observer_emit_validator_decision(&obs, &cr, &vctx, input_len);
    HU_ASSERT_EQ(cap.event_count, 0);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* ── AC-5.5: NULL observer — no crash ────────────────────────────────────── */

static void telemetry_null_observer_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain), HU_OK);

    hu_validator_context_t vctx = {0};
    vctx.persona_name = "Seth";
    vctx.persona_name_len = 4;

    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    size_t input_len = strlen(JORDAN_LEAK_F1);
    hu_error_t err =
        hu_output_validator_chain_execute(chain, &alloc, &vctx, JORDAN_LEAK_F1, input_len, &cr);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);

    /* AC-5.5 — must not crash with NULL observer. */
    hu_observer_emit_validator_decision(NULL, &cr, &vctx, input_len);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* ── NULL chain result — defensive guard ─────────────────────────────────── */

static void telemetry_null_chain_result_no_crash(void) {
    capture_ctx_t cap;
    memset(&cap, 0, sizeof(cap));
    hu_observer_t obs = make_capture_observer(&cap);
    /* Should silently no-op; no crash, no event. */
    hu_observer_emit_validator_decision(&obs, NULL, NULL, 0);
    HU_ASSERT_EQ(cap.event_count, 0);
}

/* ── Registration ─────────────────────────────────────────────────────────── */

void run_validator_telemetry_tests(void) {
    HU_TEST_SUITE("validator_telemetry");
    HU_RUN_TEST(telemetry_reject_emits_event_with_correct_decision);
    HU_RUN_TEST(telemetry_rewrite_emits_event_with_bytes_stripped);
    HU_RUN_TEST(telemetry_pass_suppressed_no_event_emitted);
    HU_RUN_TEST(telemetry_null_observer_no_crash);
    HU_RUN_TEST(telemetry_null_chain_result_no_crash);
}
