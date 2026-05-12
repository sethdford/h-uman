/* SOTA-2026 init #11 — typing simulator unit tests (S1, typing half).
 *
 * Pins:
 *   - HU_IS_TEST means no real sleeps and no real channel pulses.
 *   - Same profile + same message length ⇒ same chosen budget.
 *   - Hard ceiling = 15 s never exceeded.
 *   - Channels without start_typing/stop_typing fall back to a
 *     single-sleep budget — no fake pulses, no error.
 *   - Existing call sites that bypass `hu_typing_send` keep working
 *     (capability probe returns false on a NULL vtable).
 */

#include "human/agent/typing_simulator.h"
#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────
 * Test fixture — a stub channel that records calls.
 * ────────────────────────────────────────────────────────────────── */

typedef struct stub_ctx {
    int      start_typing_calls;
    int      stop_typing_calls;
    int      send_calls;
    char     last_message[512];
    size_t   last_message_len;
} stub_ctx_t;

static hu_error_t stub_send(void *ctx, const char *target, size_t target_len, const char *message,
                            size_t message_len, const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    stub_ctx_t *s = (stub_ctx_t *)ctx;
    s->send_calls++;
    size_t n = message_len < sizeof(s->last_message) - 1 ? message_len : sizeof(s->last_message) - 1;
    if (message && n > 0)
        memcpy(s->last_message, message, n);
    s->last_message[n] = '\0';
    s->last_message_len = n;
    return HU_OK;
}

static hu_error_t stub_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
    (void)recipient;
    (void)recipient_len;
    stub_ctx_t *s = (stub_ctx_t *)ctx;
    s->start_typing_calls++;
    return HU_OK;
}

static hu_error_t stub_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
    (void)recipient;
    (void)recipient_len;
    stub_ctx_t *s = (stub_ctx_t *)ctx;
    s->stop_typing_calls++;
    return HU_OK;
}

static const char *stub_name(void *ctx) {
    (void)ctx;
    return "stub";
}

static bool stub_health(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t kStubVtableFull = {
    .send         = stub_send,
    .name         = stub_name,
    .health_check = stub_health,
    .start_typing = stub_start_typing,
    .stop_typing  = stub_stop_typing,
};

static const hu_channel_vtable_t kStubVtableNoTyping = {
    .send         = stub_send,
    .name         = stub_name,
    .health_check = stub_health,
    /* start_typing / stop_typing intentionally NULL */
};

/* ──────────────────────────────────────────────────────────────────
 * Tests
 * ────────────────────────────────────────────────────────────────── */

static void test_typing_compute_budget_zero_wpm_is_instant(void) {
    hu_typing_profile_t p     = HU_TYPING_PROFILE_DEFAULTS;
    p.avg_wpm                 = 0;
    HU_ASSERT_EQ(hu_typing_compute_budget_ms(&p, 100), 0u);
}

static void test_typing_compute_budget_instant_override_is_zero(void) {
    hu_typing_profile_t p = HU_TYPING_PROFILE_DEFAULTS;
    p.instant             = true;
    HU_ASSERT_EQ(hu_typing_compute_budget_ms(&p, 5000), 0u);
}

static void test_typing_compute_budget_empty_message_is_zero(void) {
    hu_typing_profile_t p = HU_TYPING_PROFILE_DEFAULTS;
    HU_ASSERT_EQ(hu_typing_compute_budget_ms(&p, 0), 0u);
}

static void test_typing_compute_budget_null_profile_is_zero(void) {
    HU_ASSERT_EQ(hu_typing_compute_budget_ms(NULL, 50), 0u);
}

static void test_typing_compute_budget_deterministic_with_seed(void) {
    /* Same profile (with non-zero seed) + same message length must
     * always produce the same budget. This is the test that pins
     * the "feature 5" acceptance criterion in the brief. */
    hu_typing_profile_t p = HU_TYPING_PROFILE_DEFAULTS;
    p.seed                = 42u;
    /* Short enough that the budget stays below the ceiling so we can
     * see jitter-driven differences between seeds. 200 chars at 65
     * WPM would clamp at HU_TYPING_HARD_CEILING_MS and hide jitter. */
    uint32_t a            = hu_typing_compute_budget_ms(&p, 30);
    uint32_t b            = hu_typing_compute_budget_ms(&p, 30);
    uint32_t c            = hu_typing_compute_budget_ms(&p, 30);
    HU_ASSERT_EQ(a, b);
    HU_ASSERT_EQ(b, c);
    /* And different seeds produce different budgets at the same
     * length (jitter is real, not zero). */
    p.seed         = 99u;
    uint32_t other = hu_typing_compute_budget_ms(&p, 30);
    HU_ASSERT_NEQ(a, other);
}

static void test_typing_compute_budget_zero_seed_still_deterministic(void) {
    /* The design requires that seed=0 falls back to a *fixed*
     * default seed, NOT time(NULL), so callers that forget to set
     * a seed still see reproducible budgets. */
    hu_typing_profile_t p = HU_TYPING_PROFILE_DEFAULTS;
    p.seed                = 0u;
    uint32_t a            = hu_typing_compute_budget_ms(&p, 137);
    uint32_t b            = hu_typing_compute_budget_ms(&p, 137);
    HU_ASSERT_EQ(a, b);
}

static void test_typing_compute_budget_respects_hard_ceiling(void) {
    /* avg_wpm=20 (slow), 5 000 chars → naive estimate ≈ 75 s. The
     * ceiling must clamp this to 15 000 ms exactly. */
    hu_typing_profile_t p = HU_TYPING_PROFILE_DEFAULTS;
    p.avg_wpm             = 20;
    p.jitter_pct          = 0;
    uint32_t ms           = hu_typing_compute_budget_ms(&p, 5000);
    HU_ASSERT_LE(ms, HU_TYPING_HARD_CEILING_MS);
    HU_ASSERT_EQ(ms, HU_TYPING_HARD_CEILING_MS);
}

static void test_typing_compute_budget_grows_with_length(void) {
    hu_typing_profile_t p = HU_TYPING_PROFILE_DEFAULTS;
    p.jitter_pct          = 0;
    uint32_t short_ms     = hu_typing_compute_budget_ms(&p, 20);
    uint32_t long_ms      = hu_typing_compute_budget_ms(&p, 200);
    HU_ASSERT_GT(long_ms, short_ms);
}

static void test_typing_supports_typing_capability_probe(void) {
    hu_channel_t ch_full      = {.ctx = NULL, .vtable = &kStubVtableFull};
    hu_channel_t ch_no_typing = {.ctx = NULL, .vtable = &kStubVtableNoTyping};
    hu_channel_t ch_null      = {.ctx = NULL, .vtable = NULL};
    HU_ASSERT_TRUE(hu_channel_supports_typing(&ch_full));
    HU_ASSERT_FALSE(hu_channel_supports_typing(&ch_no_typing));
    HU_ASSERT_FALSE(hu_channel_supports_typing(&ch_null));
    HU_ASSERT_FALSE(hu_channel_supports_typing(NULL));
}

static void test_typing_send_null_args_returns_invalid(void) {
    hu_typing_profile_t p = HU_TYPING_PROFILE_DEFAULTS;
    HU_ASSERT_EQ(hu_typing_send(NULL, "t", 1, "hi", 2, NULL, 0, &p), HU_ERR_INVALID_ARGUMENT);
    hu_channel_t ch = {.ctx = NULL, .vtable = NULL};
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, "hi", 2, NULL, 0, &p), HU_ERR_INVALID_ARGUMENT);
}

static void test_typing_send_null_profile_sends_instantly(void) {
    /* The profile is optional — passing NULL must just call send()
     * once with no animation. This is the backwards-compat path:
     * existing call sites that don't know about the typing surface
     * keep working. */
    stub_ctx_t   s  = {0};
    hu_channel_t ch = {.ctx = &s, .vtable = &kStubVtableFull};
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, "hello", 5, NULL, 0, NULL), HU_OK);
    HU_ASSERT_EQ(s.send_calls, 1);
    HU_ASSERT_EQ(s.start_typing_calls, 0);
    HU_ASSERT_EQ(s.stop_typing_calls, 0);
    HU_ASSERT_STR_EQ(s.last_message, "hello");
}

static void test_typing_send_instant_profile_skips_pulses(void) {
    stub_ctx_t          s  = {0};
    hu_channel_t        ch = {.ctx = &s, .vtable = &kStubVtableFull};
    hu_typing_profile_t p  = HU_TYPING_PROFILE_DEFAULTS;
    p.instant              = true;
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, "hi", 2, NULL, 0, &p), HU_OK);
    HU_ASSERT_EQ(s.send_calls, 1);
    HU_ASSERT_EQ(s.start_typing_calls, 0);
    HU_ASSERT_EQ(s.stop_typing_calls, 0);

    /* Schedule must contain exactly one SEND action with elapsed=0. */
    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t           budget = 999u;
    size_t             n      = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_SEND);
    HU_ASSERT_EQ(acts[0].elapsed_ms, 0u);
    HU_ASSERT_EQ(budget, 0u);
}

static void test_typing_send_no_typing_channel_falls_back_instant(void) {
    /* Channel without start/stop typing must NOT raise an error and
     * must NOT call typing pulses (they don't exist). The send still
     * goes through. Under HU_IS_TEST no sleep is incurred. */
    stub_ctx_t          s  = {0};
    hu_channel_t        ch = {.ctx = &s, .vtable = &kStubVtableNoTyping};
    hu_typing_profile_t p  = HU_TYPING_PROFILE_DEFAULTS;
    p.seed                 = 7;
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, "hello there", 11, NULL, 0, &p), HU_OK);
    HU_ASSERT_EQ(s.send_calls, 1);
    HU_ASSERT_EQ(s.start_typing_calls, 0);
    HU_ASSERT_EQ(s.stop_typing_calls, 0);

    /* Schedule should be just the SEND action — no START_TYPING /
     * STOP_TYPING records because the channel doesn't support them. */
    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t           budget = 0u;
    size_t             n      = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_SEND);
    /* Budget can still be nonzero — we just don't pulse. */
    HU_ASSERT_GT(budget, 0u);
}

static void test_typing_send_typing_channel_records_pulses(void) {
    /* Short message, typing channel: expect START_TYPING + STOP_TYPING
     * + SEND in that order. */
    stub_ctx_t          s  = {0};
    hu_channel_t        ch = {.ctx = &s, .vtable = &kStubVtableFull};
    hu_typing_profile_t p  = HU_TYPING_PROFILE_DEFAULTS;
    p.seed                 = 11;
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, "hello there friend", 18, NULL, 0, &p), HU_OK);
    HU_ASSERT_EQ(s.send_calls, 1);

    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t           budget = 0u;
    size_t             n      = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_GE(n, 3u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_START_TYPING);
    HU_ASSERT_EQ((int)acts[n - 1].kind, (int)HU_TYPING_ACTION_SEND);
    /* The last STOP_TYPING must precede the SEND, never the other way. */
    HU_ASSERT_EQ((int)acts[n - 2].kind, (int)HU_TYPING_ACTION_STOP_TYPING);
    HU_ASSERT_LE(budget, HU_TYPING_HARD_CEILING_MS);
}

static void test_typing_send_long_message_refreshes_indicator(void) {
    /* For a long-enough budget, the simulator must refresh the
     * typing indicator at least once (every ~4 s). avg_wpm=30 with
     * a long message hits the 4 s threshold. */
    stub_ctx_t          s  = {0};
    hu_channel_t        ch = {.ctx = &s, .vtable = &kStubVtableFull};
    hu_typing_profile_t p  = HU_TYPING_PROFILE_DEFAULTS;
    p.avg_wpm              = 30;
    p.jitter_pct           = 0;
    p.seed                 = 5;
    /* 800 chars at 30 WPM ≈ 10.6 s budget → 2 refresh pulses + tail. */
    char buf[800];
    memset(buf, 'a', sizeof(buf));
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, buf, sizeof(buf), NULL, 0, &p), HU_OK);

    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t           budget = 0u;
    size_t             n      = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);

    int start_typing_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (acts[i].kind == HU_TYPING_ACTION_START_TYPING)
            start_typing_count++;
    }
    HU_ASSERT_GE(start_typing_count, 2);
    HU_ASSERT_LE(budget, HU_TYPING_HARD_CEILING_MS);
}

static void test_typing_send_ceiling_is_enforced(void) {
    /* Slow profile + huge message ⇒ budget clamps at 15 000 ms,
     * never exceeds. Under HU_IS_TEST this returns immediately
     * regardless of budget. */
    stub_ctx_t          s  = {0};
    hu_channel_t        ch = {.ctx = &s, .vtable = &kStubVtableFull};
    hu_typing_profile_t p  = HU_TYPING_PROFILE_DEFAULTS;
    p.avg_wpm              = 15;
    p.jitter_pct           = 0;
    p.seed                 = 13;
    char buf[4000];
    memset(buf, 'x', sizeof(buf));
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, buf, sizeof(buf), NULL, 0, &p), HU_OK);

    uint32_t budget = 0u;
    (void)hu_typing_test_take_last(NULL, 0, &budget);
    HU_ASSERT_EQ(budget, HU_TYPING_HARD_CEILING_MS);
    HU_ASSERT_EQ(s.send_calls, 1);
}

static void test_typing_send_is_deterministic(void) {
    /* Run the same call twice and assert the recorded schedule is
     * byte-identical. This is the central determinism guarantee. */
    stub_ctx_t          s1 = {0}, s2 = {0};
    hu_channel_t        ch1 = {.ctx = &s1, .vtable = &kStubVtableFull};
    hu_channel_t        ch2 = {.ctx = &s2, .vtable = &kStubVtableFull};
    hu_typing_profile_t p   = HU_TYPING_PROFILE_DEFAULTS;
    p.seed                  = 0xCAFEBABEu;
    HU_ASSERT_EQ(hu_typing_send(&ch1, "t", 1, "hello, world.", 13, NULL, 0, &p), HU_OK);

    hu_typing_action_t a1[HU_TYPING_SCHEDULE_CAP];
    uint32_t           b1 = 0u;
    size_t             n1 = hu_typing_test_take_last(a1, HU_TYPING_SCHEDULE_CAP, &b1);

    HU_ASSERT_EQ(hu_typing_send(&ch2, "t", 1, "hello, world.", 13, NULL, 0, &p), HU_OK);
    hu_typing_action_t a2[HU_TYPING_SCHEDULE_CAP];
    uint32_t           b2 = 0u;
    size_t             n2 = hu_typing_test_take_last(a2, HU_TYPING_SCHEDULE_CAP, &b2);

    HU_ASSERT_EQ(n1, n2);
    HU_ASSERT_EQ(b1, b2);
    for (size_t i = 0; i < n1; i++) {
        HU_ASSERT_EQ((int)a1[i].kind, (int)a2[i].kind);
        HU_ASSERT_EQ(a1[i].elapsed_ms, a2[i].elapsed_ms);
    }
}

static void test_typing_profile_resolve_returns_defaults(void) {
    /* NULL persona ⇒ canonical defaults regardless of channel name.
     * Init-02 (HF5 closure) only swaps in overlay-derived values when
     * a real persona is supplied; this is the legacy-call-site path. */
    hu_typing_profile_t p;
    memset(&p, 0xFF, sizeof(p));
    hu_typing_profile_resolve(NULL, "telegram", &p);
    HU_ASSERT_EQ(p.avg_wpm, 65);
    HU_ASSERT_EQ(p.wpm_stddev, 12);
    HU_ASSERT_EQ(p.pause_on_comma_ms, 180);
    HU_ASSERT_EQ(p.pause_on_period_ms, 420);
    HU_ASSERT_FALSE(p.instant);
}

/* SOTA-2026 init-02 / HF5 closure — `hu_typing_profile_resolve` now
 * actually dereferences `persona`. These tests pin the contract that
 * (a) NULL channel ⇒ defaults (used by legacy CLI tests that pass a
 * persona but no channel), (b) a missing overlay ⇒ defaults, and
 * (c) an overlay with formality / avg_length ⇒ a non-default profile. */

static void test_typing_profile_resolve_null_channel_returns_defaults(void) {
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    hu_typing_profile_t p;
    memset(&p, 0xFF, sizeof(p));
    hu_typing_profile_resolve(&persona, NULL, &p);
    HU_ASSERT_EQ(p.avg_wpm, 65);
    HU_ASSERT_EQ(p.pause_on_period_ms, 420);
}

static void test_typing_profile_resolve_missing_overlay_returns_defaults(void) {
    /* Persona with no overlays — the lookup falls through to defaults
     * without dereferencing NULL anywhere. ASan catches a stray deref
     * if the resolver regresses to ignoring the overlays-count guard. */
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    hu_typing_profile_t p;
    memset(&p, 0xFF, sizeof(p));
    hu_typing_profile_resolve(&persona, "telegram", &p);
    HU_ASSERT_EQ(p.avg_wpm, 65);
    HU_ASSERT_EQ(p.pause_on_comma_ms, 180);
}

static void test_typing_profile_resolve_formal_overlay_slows_typing(void) {
    /* HF5 evidence: a "formal" overlay on the resolved channel must
     * produce a strictly slower typing profile than defaults. This is
     * the test that pins the resolver actually using its `persona`
     * argument — the prior `(void)persona` version would silently
     * return defaults and this assertion would fail. */
    hu_persona_overlay_t overlays[1];
    memset(&overlays[0], 0, sizeof(overlays[0]));
    /* Cast away const for the storage; the resolver only reads. */
    overlays[0].channel = (char *)"slack";
    overlays[0].formality = (char *)"formal";

    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.overlays = overlays;
    persona.overlays_count = 1;

    hu_typing_profile_t p;
    memset(&p, 0xFF, sizeof(p));
    hu_typing_profile_resolve(&persona, "slack", &p);
    HU_ASSERT_LT((int)p.avg_wpm, 65);
    HU_ASSERT_GT((int)p.pause_on_period_ms, 420);
}

static void test_typing_profile_resolve_casual_short_overlay_speeds_typing(void) {
    /* Inverse of the formal case: casual + short bursts ⇒ faster than
     * defaults. Combining both knobs proves the resolver composes
     * overlay fields rather than treating one as exclusive. */
    hu_persona_overlay_t overlays[1];
    memset(&overlays[0], 0, sizeof(overlays[0]));
    overlays[0].channel = (char *)"telegram";
    overlays[0].formality = (char *)"casual";
    overlays[0].avg_length = (char *)"short";

    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.overlays = overlays;
    persona.overlays_count = 1;

    hu_typing_profile_t p;
    memset(&p, 0xFF, sizeof(p));
    hu_typing_profile_resolve(&persona, "telegram", &p);
    HU_ASSERT_GT((int)p.avg_wpm, 65);
    HU_ASSERT_LT((int)p.pause_on_period_ms, 420);
}

void run_typing_simulator_tests(void) {
    HU_TEST_SUITE("Typing");
    HU_RUN_TEST(test_typing_compute_budget_zero_wpm_is_instant);
    HU_RUN_TEST(test_typing_compute_budget_instant_override_is_zero);
    HU_RUN_TEST(test_typing_compute_budget_empty_message_is_zero);
    HU_RUN_TEST(test_typing_compute_budget_null_profile_is_zero);
    HU_RUN_TEST(test_typing_compute_budget_deterministic_with_seed);
    HU_RUN_TEST(test_typing_compute_budget_zero_seed_still_deterministic);
    HU_RUN_TEST(test_typing_compute_budget_respects_hard_ceiling);
    HU_RUN_TEST(test_typing_compute_budget_grows_with_length);
    HU_RUN_TEST(test_typing_supports_typing_capability_probe);
    HU_RUN_TEST(test_typing_send_null_args_returns_invalid);
    HU_RUN_TEST(test_typing_send_null_profile_sends_instantly);
    HU_RUN_TEST(test_typing_send_instant_profile_skips_pulses);
    HU_RUN_TEST(test_typing_send_no_typing_channel_falls_back_instant);
    HU_RUN_TEST(test_typing_send_typing_channel_records_pulses);
    HU_RUN_TEST(test_typing_send_long_message_refreshes_indicator);
    HU_RUN_TEST(test_typing_send_ceiling_is_enforced);
    HU_RUN_TEST(test_typing_send_is_deterministic);
    HU_RUN_TEST(test_typing_profile_resolve_returns_defaults);
    HU_RUN_TEST(test_typing_profile_resolve_null_channel_returns_defaults);
    HU_RUN_TEST(test_typing_profile_resolve_missing_overlay_returns_defaults);
    HU_RUN_TEST(test_typing_profile_resolve_formal_overlay_slows_typing);
    HU_RUN_TEST(test_typing_profile_resolve_casual_short_overlay_speeds_typing);
}
