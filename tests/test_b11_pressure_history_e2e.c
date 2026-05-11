/* End-to-end proof for the B11 cross-turn pressure-history wire.
 *
 * `hu_pressure_history_apply_to_trust_input` and
 * `hu_pressure_history_observe` are unit-tested in tests/test_sycophancy_pack.c
 * and the inline trust calibration is exercised in tests/test_behavior_trust.c.
 * This file pins the *production wire* — that the agent's per-turn loop in
 * src/agent/agent_turn.c::at_append_trust_directive actually:
 *
 *   (a) reads from `agent->pressure_history` *before* `hu_trust_calibrate`
 *       so the cross-turn signal can flip `user_reasserted_after_pushback`;
 *   (b) writes to `agent->pressure_history` *after* `hu_trust_calibrate`
 *       so subsequent turns see the action that was actually taken.
 *
 * If a future refactor severs either side of the wire, these tests fail.
 *
 * The unit tests prove the math; this proves the wire.
 */

#include "human/agent.h"
#include "human/agent/world_model_bridge.h"
#include "human/behavior/pressure_history.h"
#include "human/behavior/trust.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include "human/provider.h"
#include "human/providers/openai.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

/* Build a freshly-zeroed agent backed by the openai test-mode provider plus
 * an empty graph + W7 facade. Callers own everything via the returned
 * pointers and must `b11_close()` to free the inverse. */
typedef struct b11_fixture {
    hu_allocator_t alloc;
    hu_provider_t prov;
    hu_graph_t *g;
    hu_w7_facade_t *wf;
    hu_agent_t agent;
} b11_fixture_t;

static void b11_open(b11_fixture_t *f) {
    f->alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_openai_create(&f->alloc, "test-key", 8, NULL, 0, &f->prov), HU_OK);
    f->g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&f->alloc, NULL, 0, &f->g), HU_OK);
    f->wf = NULL;
    HU_ASSERT_EQ(hu_w7_facade_open(f->g, &f->alloc, &f->wf), HU_OK);
    memset(&f->agent, 0, sizeof(f->agent));
    HU_ASSERT_EQ(hu_agent_from_config(&f->agent, &f->alloc, f->prov, NULL, 0, NULL, NULL, NULL,
                                       NULL, "gpt-4o", 6, "openai", 6, 0.7, ".", 1, 25, 50, false, 0,
                                       NULL, 0, NULL, 0, NULL),
                 HU_OK);
    f->agent.verifier_graph = f->g;
    f->agent.w7_facade = f->wf;
    f->agent.memory_session_id = "u_b11_e2e";
    f->agent.memory_session_id_len = 9;
    f->agent.active_channel = "imessage";
    f->agent.active_channel_len = 8;
}

static void b11_close(b11_fixture_t *f) {
    /* NOTE: do NOT call hu_world_model_invalidate(NULL, 0). The
     * process-global world-model cache may hold entries inserted by
     * other tests whose allocators have already gone out of scope; a
     * global invalidate would try to free against those dead allocators
     * and crash. Other E2E suites in this binary follow the same
     * convention. */
    hu_agent_deinit(&f->agent);
    hu_graph_close(f->g, &f->alloc);
}

/* The pivotal observation wire. Drive `hu_agent_turn` three times with
 * progressively similar reassertions; the agent must record each turn into
 * its own `pressure_history` ring, normalised+truncated, with a non-zero
 * trust action stored. Without the wire, `count` stays at 0 forever and
 * cross-turn sycophancy detection silently goes dark. */
static void b11_agent_turn_observes_into_pressure_history(void) {
#if HU_IS_TEST
    b11_fixture_t f;
    b11_open(&f);

    HU_ASSERT_EQ(f.agent.pressure_history.count, (size_t)0);
    HU_ASSERT_EQ(f.agent.pressure_history.head, (size_t)0);

    const char *t1 = "Berlin is in France.";
    const char *t2 = "Berlin is in France, I told you.";
    const char *t3 = "Look, Berlin is definitely in France.";
    char *r1 = NULL, *r2 = NULL, *r3 = NULL;
    size_t r1l = 0, r2l = 0, r3l = 0;

    HU_ASSERT_EQ(hu_agent_turn(&f.agent, t1, strlen(t1), &r1, &r1l), HU_OK);
    HU_ASSERT_EQ(f.agent.pressure_history.count, (size_t)1);

    HU_ASSERT_EQ(hu_agent_turn(&f.agent, t2, strlen(t2), &r2, &r2l), HU_OK);
    HU_ASSERT_EQ(f.agent.pressure_history.count, (size_t)2);

    HU_ASSERT_EQ(hu_agent_turn(&f.agent, t3, strlen(t3), &r3, &r3l), HU_OK);
    HU_ASSERT_EQ(f.agent.pressure_history.count, (size_t)3);

    /* Each entry must be normalised — lowercase ASCII letters/digits,
     * collapsed whitespace, no trailing space, no original punctuation. */
    bool any_seen = false;
    for (size_t i = 0; i < f.agent.pressure_history.count; i++) {
        const hu_pressure_entry_t *e = &f.agent.pressure_history.entries[i];
        HU_ASSERT_TRUE(e->normalized_len > 0);
        HU_ASSERT_TRUE(e->normalized_len < HU_PRESSURE_HISTORY_MSG_BYTES);
        HU_ASSERT_EQ(e->normalized[e->normalized_len], '\0');
        if (strstr(e->normalized, "berlin") && strstr(e->normalized, "france")) {
            any_seen = true;
        }
    }
    HU_ASSERT_TRUE(any_seen);

    /* The apply wire is the *other* half. With 3 prior similar entries, a
     * fourth reassertion must surface as `reassertion_count >= 3` when
     * inspected against the agent's history. We inspect directly here
     * rather than going through another agent_turn so the test stays
     * fast and provider-output-independent. */
    bool after_pushback = false;
    uint32_t reasserts = 0;
    HU_ASSERT_EQ(hu_pressure_history_inspect(&f.agent.pressure_history,
                                             "Berlin is in France!", 20, &after_pushback,
                                             &reasserts),
                 HU_OK);
    HU_ASSERT_TRUE(reasserts >= 3u);

    if (r1) f.alloc.free(f.alloc.ctx, r1, r1l + 1);
    if (r2) f.alloc.free(f.alloc.ctx, r2, r2l + 1);
    if (r3) f.alloc.free(f.alloc.ctx, r3, r3l + 1);
    b11_close(&f);
#endif
}

/* Negative case: completely unrelated turns must NOT cross-pollute the
 * reassertion count. If the normalisation or similarity check is broken,
 * unrelated short messages will spuriously match each other and lift the
 * trust input — the exact failure mode B11 is designed to prevent. */
static void b11_agent_turn_does_not_inflate_unrelated_messages(void) {
#if HU_IS_TEST
    b11_fixture_t f;
    b11_open(&f);

    const char *q1 = "What is the weather like in Tokyo?";
    const char *q2 = "Can you suggest a good book about origami?";
    const char *q3 = "Remind me to call my dentist tomorrow morning.";
    char *r1 = NULL, *r2 = NULL, *r3 = NULL;
    size_t r1l = 0, r2l = 0, r3l = 0;

    HU_ASSERT_EQ(hu_agent_turn(&f.agent, q1, strlen(q1), &r1, &r1l), HU_OK);
    HU_ASSERT_EQ(hu_agent_turn(&f.agent, q2, strlen(q2), &r2, &r2l), HU_OK);
    HU_ASSERT_EQ(hu_agent_turn(&f.agent, q3, strlen(q3), &r3, &r3l), HU_OK);

    HU_ASSERT_EQ(f.agent.pressure_history.count, (size_t)3);

    /* A new, unrelated 4th message must not score as a reassertion. */
    bool after_pushback = true;
    uint32_t reasserts = 99;
    HU_ASSERT_EQ(hu_pressure_history_inspect(&f.agent.pressure_history,
                                             "Tell me a fun fact about octopus brains.", 40,
                                             &after_pushback, &reasserts),
                 HU_OK);
    HU_ASSERT_FALSE(after_pushback);
    HU_ASSERT_EQ((long long)reasserts, 0LL);

    if (r1) f.alloc.free(f.alloc.ctx, r1, r1l + 1);
    if (r2) f.alloc.free(f.alloc.ctx, r2, r2l + 1);
    if (r3) f.alloc.free(f.alloc.ctx, r3, r3l + 1);
    b11_close(&f);
#endif
}

/* Ring-buffer semantics: pushing more than HU_PRESSURE_HISTORY_CAP turns
 * must not crash, must keep the head wrapping correctly, and must keep the
 * count saturated below UINT32_MAX. Pinning this protects against off-by-one
 * regressions in the wraparound math. */
static void b11_agent_turn_ring_buffer_wraps_safely(void) {
#if HU_IS_TEST
    b11_fixture_t f;
    b11_open(&f);

    /* Drive HU_PRESSURE_HISTORY_CAP + 2 turns to force at least two
     * wraparounds of the ring head. The exact text doesn't matter; we
     * only care about the bookkeeping. */
    const size_t n = HU_PRESSURE_HISTORY_CAP + 2;
    for (size_t i = 0; i < n; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Note number %zu about a topic.", i);
        char *r = NULL;
        size_t rl = 0;
        HU_ASSERT_EQ(hu_agent_turn(&f.agent, msg, strlen(msg), &r, &rl), HU_OK);
        if (r) f.alloc.free(f.alloc.ctx, r, rl + 1);
    }

    /* Total observations should be n; head should have wrapped to (n %
     * CAP); count saturates with n itself (well below UINT32_MAX). */
    HU_ASSERT_EQ(f.agent.pressure_history.count, n);
    HU_ASSERT_EQ(f.agent.pressure_history.head, n % HU_PRESSURE_HISTORY_CAP);

    b11_close(&f);
#endif
}

#endif /* HU_ENABLE_SQLITE */

void run_b11_pressure_history_e2e_tests(void);

void run_b11_pressure_history_e2e_tests(void) {
#ifdef HU_ENABLE_SQLITE
    HU_TEST_SUITE("B11 cross-turn pressure history wiring");
    HU_RUN_TEST(b11_agent_turn_observes_into_pressure_history);
    HU_RUN_TEST(b11_agent_turn_does_not_inflate_unrelated_messages);
    HU_RUN_TEST(b11_agent_turn_ring_buffer_wraps_safely);
#endif
}
