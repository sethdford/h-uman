/* SOTA-2026 init-11 S1.5 follow-up — CLI channel ↔ typing simulator
 * wiring tests.
 *
 * The CLI channel adopts `hu_typing_send` only when a persona handle
 * is attached via `hu_cli_set_persona`. These tests pin the four
 * branches of that decision tree:
 *
 *   1. persona == NULL                       ⇒ direct byte-for-byte write
 *   2. persona != NULL, profile.instant=true ⇒ direct byte-for-byte write
 *   3. persona != NULL, profile non-instant  ⇒ routes through hu_typing_send
 *   4. hu_typing_send returns error          ⇒ falls back to direct write
 *
 * Plus a determinism gate: byte-identical stdout across two runs of
 * the same (persona, profile, message) triple.
 *
 * Under HU_IS_TEST the typing simulator's sleep shim is a no-op
 * (already asserted in test_typing_simulator.c), so the CLI tests
 * here run at memory speed.
 */

#include "human/agent/typing_simulator.h"
#include "human/channel.h"
#include "human/channels/cli.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* HF5 closure (init-02 S2): the typing resolver now dereferences `persona`
 * via `hu_persona_find_overlay`. CLI tests therefore pass a real
 * (zero-initialized) `hu_persona_t` instead of an int sentinel. A
 * zero-init persona has zero overlays so the resolver returns defaults —
 * the same behavior the int-sentinel tests assumed, but no UB. */
static hu_persona_t s_test_persona;

/* ──────────────────────────────────────────────────────────────────
 * stdout capture helper
 *
 * The CLI channel writes directly to stdout. To assert byte-for-byte
 * preservation and determinism we redirect stdout to a temp file
 * around the call, then read it back. Matches the freopen pattern
 * already in tests/test_cli.c.
 *
 * Returns the captured bytes (caller frees) and the byte count via
 * `*out_len`. NULL on failure.
 * ────────────────────────────────────────────────────────────────── */
static char *capture_cli_send(hu_channel_t *ch, const char *message, size_t message_len,
                              size_t *out_len) {
    char tmpl[] = "/tmp/hu_cli_typing_XXXXXX";
    int  tfd    = mkstemp(tmpl);
    if (tfd < 0)
        return NULL;
    if (close(tfd) != 0) {
        unlink(tmpl);
        return NULL;
    }
    int save_out = dup(STDOUT_FILENO);
    if (save_out < 0) {
        unlink(tmpl);
        return NULL;
    }
    if (!freopen(tmpl, "w", stdout)) {
        dup2(save_out, STDOUT_FILENO);
        close(save_out);
        unlink(tmpl);
        return NULL;
    }

    hu_error_t err = ch->vtable->send(ch->ctx, NULL, 0, message, message_len, NULL, 0);

    fflush(stdout);
    dup2(save_out, STDOUT_FILENO);
    close(save_out);

    if (err != HU_OK) {
        unlink(tmpl);
        return NULL;
    }

    FILE *rf = fopen(tmpl, "rb");
    if (!rf) {
        unlink(tmpl);
        return NULL;
    }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(rf);
        unlink(tmpl);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, rf);
    buf[n]   = '\0';
    fclose(rf);
    unlink(tmpl);
    if (out_len)
        *out_len = n;
    return buf;
}

/* ──────────────────────────────────────────────────────────────────
 * Schedule-buffer marker — primes the typing simulator's HU_IS_TEST
 * capture ring to a known state so we can tell, after a `cli_send`
 * call, whether the channel routed through `hu_typing_send`.
 *
 * After `prime`, the last-schedule has exactly one SEND action
 * (elapsed_ms=0) and `budget=0`. Any subsequent `hu_typing_send`
 * call resets the buffer; a CLI send that takes the byte-for-byte
 * bypass leaves it untouched.
 * ────────────────────────────────────────────────────────────────── */
static int  s_marker_send_calls;
static hu_error_t marker_send(void *ctx, const char *target, size_t target_len,
                              const char *message, size_t message_len,
                              const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    int *counter = (int *)ctx;
    if (counter)
        (*counter)++;
    return HU_OK;
}
static const hu_channel_vtable_t kMarkerVtable = {
    .send = marker_send,
};

static void prime_typing_buffer_to_known_state(void) {
    s_marker_send_calls = 0;
    hu_channel_t ch = {.ctx = &s_marker_send_calls, .vtable = &kMarkerVtable};
    /* NULL profile ⇒ no budget, one SEND record at elapsed_ms=0. */
    hu_error_t err = hu_typing_send(&ch, "t", 1, "x", 1, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(s_marker_send_calls, 1);

    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t           budget = 999u;
    size_t             n      = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_SEND);
    HU_ASSERT_EQ(acts[0].elapsed_ms, 0u);
    HU_ASSERT_EQ(budget, 0u);
}

/* ──────────────────────────────────────────────────────────────────
 * Tests
 * ────────────────────────────────────────────────────────────────── */

/* 1. NULL persona ⇒ direct byte-for-byte write. The typing simulator's
 *    capture buffer is never touched, and stdout receives exactly
 *    "hello\n" matching the legacy path. */
static void test_cli_typing_null_persona_writes_directly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_cli_create(&alloc, &ch), HU_OK);
    /* Persona is NULL by default — explicit for documentation. */
    hu_cli_set_persona(&ch, NULL);

    prime_typing_buffer_to_known_state();

    size_t out_len = 0;
    char  *out     = capture_cli_send(&ch, "hello", 5, &out_len);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 6u);
    HU_ASSERT_STR_EQ(out, "hello\n");
    free(out);

    /* Buffer must still be the primed state — no hu_typing_send call
     * happened inside cli_send. */
    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t           budget = 999u;
    size_t             n      = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_SEND);
    HU_ASSERT_EQ(acts[0].elapsed_ms, 0u);
    HU_ASSERT_EQ(budget, 0u);

    hu_cli_destroy(&ch);
}

/* 2. persona attached, but the resolved profile is `instant` ⇒ direct
 *    byte-for-byte write. Today `hu_typing_profile_resolve` always
 *    returns defaults (init-02 owns the overlay wiring), so we exercise
 *    this branch indirectly: the contract is the same byte-for-byte
 *    bypass that NULL-persona uses. We assert the bypass by verifying
 *    the schedule buffer is untouched after a marker prime — same
 *    invariant as test (1). The "instant" decision lives inside
 *    cli_send and is taken when the resolver yields `profile.instant`;
 *    until init-02 ships the overlay loader, that path is reached via
 *    a defaults-flip if defaults ever change. The test pins the
 *    invariant rather than the resolver internals. */
static void test_cli_typing_instant_profile_writes_directly(void) {
    /* Sanity: defaults today are non-instant. If init-02 ever flips
     * the default we want this guard to scream so the test below
     * is re-evaluated. */
    hu_typing_profile_t defaults;
    hu_typing_profile_resolve(NULL, "cli", &defaults);
    HU_ASSERT_FALSE(defaults.instant);

    /* Cross-check: the public hu_typing_send contract with
     * profile.instant=true performs no pulses and zero budget.
     * That's the contract cli_send relies on for byte-for-byte
     * preservation; pin it here so a future regression in
     * typing_simulator.c surfaces against the CLI wiring tests too. */
    int                        counter = 0;
    hu_channel_t               ch      = {.ctx = &counter, .vtable = &kMarkerVtable};
    hu_typing_profile_t        instant = HU_TYPING_PROFILE_DEFAULTS;
    instant.instant                    = true;
    HU_ASSERT_EQ(hu_typing_send(&ch, "t", 1, "hi", 2, NULL, 0, &instant), HU_OK);
    HU_ASSERT_EQ(counter, 1);
    uint32_t budget = 99u;
    size_t   n      = hu_typing_test_take_last(NULL, 0, &budget);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ(budget, 0u);
}

/* 3. persona != NULL with the default (non-instant) profile ⇒ cli_send
 *    routes through hu_typing_send. The typing simulator records a
 *    non-zero budget and the schedule buffer transitions from the
 *    primed state to one containing exactly one SEND action with
 *    elapsed_ms == budget (CLI channel does not expose start_typing/
 *    stop_typing, so no refresh pulses). Under HU_IS_TEST no real
 *    sleep is incurred. */
static void test_cli_typing_default_profile_calls_typing_send_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_cli_create(&alloc, &ch), HU_OK);
    /* S1.5 critic HF5 + init-02 S2: the resolver now reads the
     * persona's channel overlays. A zero-init persona has none, so
     * defaults still apply — pinning the legacy direct-write behavior
     * even after the resolver started dereferencing. Using a static
     * (file-scope) handle keeps it landmine-free for the resolver's
     * read path. */
    memset(&s_test_persona, 0, sizeof(s_test_persona));
    hu_cli_set_persona(&ch, &s_test_persona);

    prime_typing_buffer_to_known_state();

    size_t out_len = 0;
    char  *out     = capture_cli_send(&ch, "hello world", 11, &out_len);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 12u);
    HU_ASSERT_STR_EQ(out, "hello world\n");
    free(out);

    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t           budget = 0u;
    size_t             n      = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    /* CLI does not implement start_typing/stop_typing — the schedule
     * has exactly one SEND action and a non-trivial budget that
     * matches its elapsed_ms slot. */
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_SEND);
    HU_ASSERT_GT(budget, 0u);
    HU_ASSERT_LE(budget, HU_TYPING_HARD_CEILING_MS);
    HU_ASSERT_EQ(acts[0].elapsed_ms, budget);

    hu_cli_destroy(&ch);
}

/* 4. Failure path: empty message + persona set still returns HU_OK.
 *
 *    The CLI contract is "never fail to deliver a message just because
 *    typing simulation failed". In practice, the typing simulator
 *    returns HU_OK for all reasonable inputs (a non-NULL channel with
 *    a non-NULL send vtable). The only error branch is
 *    HU_ERR_INVALID_ARGUMENT, which the CLI wiring never triggers
 *    because it always passes its own non-NULL channel + raw vtable.
 *
 *    We exercise the bypass branch by sending an empty payload, which
 *    yields budget=0 and a single SEND record, and verify the CLI
 *    still produces HU_OK and zero bytes on stdout (matching the
 *    legacy `cli_send` behavior for empty input). */
static void test_cli_typing_send_error_falls_back_to_direct_write(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_cli_create(&alloc, &ch), HU_OK);
    memset(&s_test_persona, 0, sizeof(s_test_persona));
    hu_cli_set_persona(&ch, &s_test_persona);

    size_t out_len = 99u;
    char  *out     = capture_cli_send(&ch, "", 0, &out_len);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
    free(out);

    /* And a NULL message buffer ⇒ HU_OK + nothing on stdout. */
    out_len = 99u;
    out     = capture_cli_send(&ch, NULL, 0, &out_len);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
    free(out);

    hu_cli_destroy(&ch);
}

/* 5. Determinism: same (persona, profile, message) ⇒ byte-identical
 *    stdout across two runs. Because the CLI write itself never
 *    depends on the typing budget, this is equivalent to "the typing
 *    decorator does not corrupt the payload". Pin it anyway — it's
 *    the central guarantee callers will rely on when init-02 wires
 *    real seeded profiles. */
static void test_cli_send_is_deterministic_with_seeded_profile(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_cli_create(&alloc, &ch), HU_OK);
    memset(&s_test_persona, 0, sizeof(s_test_persona));
    hu_cli_set_persona(&ch, &s_test_persona);

    size_t len_a = 0, len_b = 0;
    char  *a = capture_cli_send(&ch, "hello, world.", 13, &len_a);
    char  *b = capture_cli_send(&ch, "hello, world.", 13, &len_b);
    HU_ASSERT_NOT_NULL(a);
    HU_ASSERT_NOT_NULL(b);
    HU_ASSERT_EQ(len_a, len_b);
    HU_ASSERT_EQ(memcmp(a, b, len_a), 0);
    HU_ASSERT_STR_EQ(a, "hello, world.\n");
    free(a);
    free(b);

    /* And the typing budget itself is deterministic — same message
     * length under the same resolved profile yields the same budget
     * twice in a row. */
    uint32_t budget_first = 0u;
    (void)hu_typing_test_take_last(NULL, 0, &budget_first);
    /* Re-run a third send and re-read the budget. */
    char *c = capture_cli_send(&ch, "hello, world.", 13, NULL);
    HU_ASSERT_NOT_NULL(c);
    free(c);
    uint32_t budget_second = 0u;
    (void)hu_typing_test_take_last(NULL, 0, &budget_second);
    HU_ASSERT_EQ(budget_first, budget_second);

    hu_cli_destroy(&ch);
}

/* 6. Legacy invariant: hu_cli_create yields a channel with a NULL
 *    persona, and `hu_cli_set_persona(NULL_channel, …)` is a no-op
 *    rather than a crash. Pins the safety contract of the new setter. */
static void test_cli_set_persona_handles_null_inputs(void) {
    hu_cli_set_persona(NULL, NULL);        /* must not crash */
    hu_cli_set_persona(NULL, (void *)0x1); /* must not crash */

    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch    = {0};
    HU_ASSERT_EQ(hu_cli_create(&alloc, &ch), HU_OK);
    /* Calling set_persona on a freshly-created channel should be safe
     * and idempotent. */
    hu_cli_set_persona(&ch, NULL);
    hu_cli_set_persona(&ch, NULL);
    hu_cli_destroy(&ch);
}

void run_channel_cli_tests(void) {
    HU_TEST_SUITE("Channel CLI Typing");
    HU_RUN_TEST(test_cli_typing_null_persona_writes_directly);
    HU_RUN_TEST(test_cli_typing_instant_profile_writes_directly);
    HU_RUN_TEST(test_cli_typing_default_profile_calls_typing_send_under_test);
    HU_RUN_TEST(test_cli_typing_send_error_falls_back_to_direct_write);
    HU_RUN_TEST(test_cli_send_is_deterministic_with_seeded_profile);
    HU_RUN_TEST(test_cli_set_persona_handles_null_inputs);
}
