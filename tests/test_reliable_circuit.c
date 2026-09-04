/* src/providers/reliable.c — fail-fast against a dead-but-listening primary.
 *
 * Incident 2026-09-03 04:31: mlx-server on :8741 died but its socket kept
 * accepting; every daemon request hung to the 300 s low-speed timeout, was
 * retried twice more, and only then reached the cloud fallback — the service
 * loop sat wedged in 5-minute blocks for hours. These tests pin the two rules
 * that end that: a timed-out provider is not retried within a call, and after
 * N consecutive primary failures the primary is skipped for a recovery window
 * (one trial request afterwards). A scripted fake provider and an injected
 * clock make every branch deterministic. */
#include "human/config.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/providers/reliable.h"
#include "test_framework.h"

#include <string.h>
#include <time.h>

/* ── Scripted fake provider ─────────────────────────────────────────── */

typedef struct fake_provider {
    const char *name;
    int calls;        /* every chat_with_system call, failing or not */
    int fail_first_n; /* calls 1..n fail with fail_err; later calls succeed */
    hu_error_t fail_err;
} fake_provider_t;

static hu_error_t fake_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
                                        size_t system_prompt_len, const char *message,
                                        size_t message_len, const char *model, size_t model_len,
                                        double temperature, char **out, size_t *out_len) {
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    fake_provider_t *f = (fake_provider_t *)ctx;
    f->calls++;
    if (f->calls <= f->fail_first_n)
        return f->fail_err;
    size_t n = strlen(f->name);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, f->name, n + 1);
    *out = buf;
    *out_len = n;
    return HU_OK;
}

static const char *fake_get_name(void *ctx) {
    return ((fake_provider_t *)ctx)->name;
}

static void fake_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t fake_vtable = {
    .chat_with_system = fake_chat_with_system,
    .get_name = fake_get_name,
    .deinit = fake_deinit,
};

static time_t g_fake_now = 1000;
static time_t fake_clock(void *ud) {
    (void)ud;
    return g_fake_now;
}

typedef struct rig {
    hu_allocator_t alloc;
    fake_provider_t prim;
    fake_provider_t fb;
    hu_reliable_provider_entry_t extras[1];
    hu_provider_t reliable;
} rig_t;

/* Primary fails its first `prim_fail_n` calls with `prim_err`; fallback always
 * succeeds; max_retries retries per provider; clock injected at g_fake_now. */
static void rig_init(rig_t *g, int prim_fail_n, hu_error_t prim_err, uint32_t max_retries) {
    memset(g, 0, sizeof(*g));
    g->alloc = hu_system_allocator();
    g->prim.name = "primary";
    g->prim.fail_first_n = prim_fail_n;
    g->prim.fail_err = prim_err;
    g->fb.name = "fallback";
    hu_provider_t prim = {.ctx = &g->prim, .vtable = &fake_vtable};
    hu_provider_t fb = {.ctx = &g->fb, .vtable = &fake_vtable};
    g->extras[0].name = "fallback";
    g->extras[0].name_len = 8;
    g->extras[0].provider = fb;
    HU_ASSERT_EQ(hu_reliable_create_ex(&g->alloc, prim, max_retries, 50, g->extras, 1, NULL, 0,
                                       &g->reliable),
                 HU_OK);
    g_fake_now = 1000;
    hu_reliable_set_clock(&g->reliable, fake_clock, NULL);
}

static hu_error_t rig_call(rig_t *g, char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return g->reliable.vtable->chat_with_system(g->reliable.ctx, &g->alloc, "sys", 3, "hi", 2, "m",
                                                1, 0.5, out, out_len);
}

static void rig_free_out(rig_t *g, char *out, size_t out_len) {
    if (out)
        g->alloc.free(g->alloc.ctx, out, out_len + 1);
}

static void rig_deinit(rig_t *g) {
    g->reliable.vtable->deinit(g->reliable.ctx, &g->alloc);
}

/* ── 1. the predicate ───────────────────────────────────────────────── */

static void test_predicate_only_timeout_ends_provider_attempts(void) {
    HU_ASSERT(hu_reliable_error_ends_provider_attempts(HU_ERR_TIMEOUT));
    HU_ASSERT(!hu_reliable_error_ends_provider_attempts(HU_ERR_IO));
    HU_ASSERT(!hu_reliable_error_ends_provider_attempts(HU_ERR_PROVIDER_RESPONSE));
    HU_ASSERT(!hu_reliable_error_ends_provider_attempts(HU_ERR_PROVIDER_RATE_LIMITED));
    HU_ASSERT(!hu_reliable_error_ends_provider_attempts(HU_OK));
}

/* ── 2. a timeout is not retried on the same provider ───────────────── */

static void test_timeout_is_not_retried_and_falls_to_fallback(void) {
    rig_t g;
    rig_init(&g, 100, HU_ERR_TIMEOUT, 2); /* primary always times out */
    char *out;
    size_t out_len;
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
    HU_ASSERT_EQ(g.prim.calls, 1); /* pre-fix: 3 (2 retries × 300 s each) */
    HU_ASSERT_EQ(g.fb.calls, 1);
    HU_ASSERT_STR_EQ(out, "fallback");
    rig_free_out(&g, out, out_len);
    rig_deinit(&g);
}

static void test_non_timeout_error_still_retries_same_provider(void) {
    rig_t g;
    rig_init(&g, 1, HU_ERR_IO, 2); /* one transient failure, then success */
    char *out;
    size_t out_len;
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
    HU_ASSERT_EQ(g.prim.calls, 2); /* retried once and recovered */
    HU_ASSERT_EQ(g.fb.calls, 0);
    HU_ASSERT_STR_EQ(out, "primary");
    rig_free_out(&g, out, out_len);
    rig_deinit(&g);
}

/* ── 3. the circuit opens, holds, half-opens, closes ────────────────── */

static void test_circuit_opens_after_default_threshold_and_skips_primary(void) {
    rig_t g;
    rig_init(&g, 100, HU_ERR_TIMEOUT, 2);
    char *out;
    size_t out_len;
    int failures = -1;
    time_t open_until = -1;

    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK); /* failure 1 */
    rig_free_out(&g, out, out_len);
    hu_reliable_circuit_state(&g.reliable, &failures, &open_until);
    HU_ASSERT_EQ(failures, 1);
    HU_ASSERT_EQ((long long)open_until, 0LL); /* still closed */

    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK); /* failure 2 → OPEN */
    rig_free_out(&g, out, out_len);
    hu_reliable_circuit_state(&g.reliable, &failures, &open_until);
    HU_ASSERT_EQ(failures, HU_RELIABLE_CIRCUIT_DEFAULT_THRESHOLD);
    HU_ASSERT_EQ((long long)open_until, 1000LL + HU_RELIABLE_CIRCUIT_DEFAULT_RECOVERY_SECS);
    HU_ASSERT_EQ(g.prim.calls, 2);

    /* Open: the primary is not even contacted. */
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
    rig_free_out(&g, out, out_len);
    HU_ASSERT_EQ(g.prim.calls, 2);
    HU_ASSERT_EQ(g.fb.calls, 3);

    g_fake_now = 1000 + HU_RELIABLE_CIRCUIT_DEFAULT_RECOVERY_SECS - 1; /* one second early */
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
    rig_free_out(&g, out, out_len);
    HU_ASSERT_EQ(g.prim.calls, 2);
    rig_deinit(&g);
}

static void test_circuit_half_open_success_closes_it(void) {
    rig_t g;
    rig_init(&g, 2, HU_ERR_TIMEOUT, 2); /* times out twice, then healthy */
    char *out;
    size_t out_len;
    for (int i = 0; i < 2; i++) {
        HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
        rig_free_out(&g, out, out_len);
    }
    HU_ASSERT_EQ(g.prim.calls, 2); /* open now */

    g_fake_now = 1000 + HU_RELIABLE_CIRCUIT_DEFAULT_RECOVERY_SECS; /* recovery elapsed */
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
    HU_ASSERT_EQ(g.prim.calls, 3); /* the one trial request */
    HU_ASSERT_STR_EQ(out, "primary");
    rig_free_out(&g, out, out_len);

    int failures = -1;
    time_t open_until = -1;
    hu_reliable_circuit_state(&g.reliable, &failures, &open_until);
    HU_ASSERT_EQ(failures, 0);
    HU_ASSERT_EQ((long long)open_until, 0LL);

    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK); /* closed: primary again */
    HU_ASSERT_EQ(g.prim.calls, 4);
    rig_free_out(&g, out, out_len);
    rig_deinit(&g);
}

static void test_circuit_half_open_failure_reopens_for_another_window(void) {
    rig_t g;
    rig_init(&g, 100, HU_ERR_TIMEOUT, 2); /* never recovers */
    char *out;
    size_t out_len;
    for (int i = 0; i < 2; i++) {
        HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
        rig_free_out(&g, out, out_len);
    }
    g_fake_now = 1000 + HU_RELIABLE_CIRCUIT_DEFAULT_RECOVERY_SECS;
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK); /* trial → fails → fallback */
    rig_free_out(&g, out, out_len);
    HU_ASSERT_EQ(g.prim.calls, 3);
    int failures = -1;
    time_t open_until = -1;
    hu_reliable_circuit_state(&g.reliable, &failures, &open_until);
    HU_ASSERT_EQ(failures, 3);
    HU_ASSERT_EQ((long long)open_until,
                 (long long)g_fake_now + HU_RELIABLE_CIRCUIT_DEFAULT_RECOVERY_SECS);

    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK); /* open again: primary skipped */
    rig_free_out(&g, out, out_len);
    HU_ASSERT_EQ(g.prim.calls, 3);
    rig_deinit(&g);
}

/* ── 4. configuration of the circuit ────────────────────────────────── */

static void test_set_circuit_negative_threshold_disables(void) {
    rig_t g;
    rig_init(&g, 100, HU_ERR_TIMEOUT, 0);
    hu_reliable_set_circuit(&g.reliable, -1, 0);
    char *out;
    size_t out_len;
    for (int i = 0; i < 4; i++) {
        HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
        rig_free_out(&g, out, out_len);
    }
    HU_ASSERT_EQ(g.prim.calls, 4); /* never skipped */
    int failures = -1;
    time_t open_until = -1;
    hu_reliable_circuit_state(&g.reliable, &failures, &open_until);
    HU_ASSERT_EQ(failures, 0);
    HU_ASSERT_EQ((long long)open_until, 0LL);
    rig_deinit(&g);
}

static void test_set_circuit_zero_keeps_defaults_and_custom_threshold_applies(void) {
    rig_t g;
    rig_init(&g, 100, HU_ERR_TIMEOUT, 0);
    hu_reliable_set_circuit(&g.reliable, 0, 0); /* no-op */
    char *out;
    size_t out_len;
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
    rig_free_out(&g, out, out_len);
    int failures = -1;
    time_t open_until = -1;
    hu_reliable_circuit_state(&g.reliable, &failures, &open_until);
    HU_ASSERT_EQ((long long)open_until, 0LL); /* default threshold 2: still closed */
    rig_deinit(&g);

    rig_init(&g, 100, HU_ERR_TIMEOUT, 0);
    hu_reliable_set_circuit(&g.reliable, 1, 60);
    HU_ASSERT_EQ(rig_call(&g, &out, &out_len), HU_OK);
    rig_free_out(&g, out, out_len);
    hu_reliable_circuit_state(&g.reliable, &failures, &open_until);
    HU_ASSERT_EQ(failures, 1);
    HU_ASSERT_EQ((long long)open_until, 1060LL);
    rig_deinit(&g);
}

static void test_config_parses_circuit_keys_with_absent_as_zero(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);

    const char *absent = "{\"reliability\":{\"provider_retries\":2}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, absent, strlen(absent)), HU_OK);
    HU_ASSERT_EQ(cfg.reliability.circuit_failure_threshold, 0); /* → defaults */
    HU_ASSERT_EQ(cfg.reliability.circuit_recovery_secs, 0);

    const char *set = "{\"reliability\":{\"circuit_failure_threshold\":3,"
                      "\"circuit_recovery_secs\":120}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, set, strlen(set)), HU_OK);
    HU_ASSERT_EQ(cfg.reliability.circuit_failure_threshold, 3);
    HU_ASSERT_EQ(cfg.reliability.circuit_recovery_secs, 120);

    const char *off = "{\"reliability\":{\"circuit_failure_threshold\":-1}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, off, strlen(off)), HU_OK);
    HU_ASSERT_EQ(cfg.reliability.circuit_failure_threshold, -1);

    const char *junk = "{\"reliability\":{\"circuit_failure_threshold\":9999}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, junk, strlen(junk)), HU_OK);
    HU_ASSERT_EQ(cfg.reliability.circuit_failure_threshold, -1); /* out of range: unchanged */
    hu_arena_destroy(arena);
}

void run_reliable_circuit_tests(void) {
    HU_TEST_SUITE("Reliable Circuit Breaker");
    HU_RUN_TEST(test_predicate_only_timeout_ends_provider_attempts);
    HU_RUN_TEST(test_timeout_is_not_retried_and_falls_to_fallback);
    HU_RUN_TEST(test_non_timeout_error_still_retries_same_provider);
    HU_RUN_TEST(test_circuit_opens_after_default_threshold_and_skips_primary);
    HU_RUN_TEST(test_circuit_half_open_success_closes_it);
    HU_RUN_TEST(test_circuit_half_open_failure_reopens_for_another_window);
    HU_RUN_TEST(test_set_circuit_negative_threshold_disables);
    HU_RUN_TEST(test_set_circuit_zero_keeps_defaults_and_custom_threshold_applies);
    HU_RUN_TEST(test_config_parses_circuit_keys_with_absent_as_zero);
}
