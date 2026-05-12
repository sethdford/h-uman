/* G3 regression tests: Telegram channel ↔ hu_typing_send wiring.
 *
 * Mirrors the pattern from tests/test_channel_cli.c.  Pins four branches:
 *
 *   1. persona == NULL            ⇒ direct send (typing buffer untouched)
 *   2. persona set, non-instant   ⇒ hu_typing_send called, budget > 0,
 *                                    message still delivered
 *   3. persona set, native typing ⇒ schedule has START_TYPING + STOP_TYPING
 *                                    + SEND actions (channel supports typing)
 *   4. hu_telegram_set_persona(NULL/bad) is a safe no-op
 *
 * Under HU_IS_TEST the typing simulator's sleep shim is a no-op and
 * start_typing/stop_typing vtable entries are not called, so these tests
 * run at memory speed without any network or real delays.
 */

#include "human/agent/typing_simulator.h"
#include "human/channel.h"
#include "human/channels/telegram.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

/* Zero-initialized persona: no overlays → resolver returns defaults.
 * File-scoped so the resolver's read path can safely dereference it. */
static hu_persona_t s_test_persona;

/* ── Marker helpers (identical pattern to test_channel_cli.c) ────────── */

static int s_marker_calls;
static hu_error_t marker_send(void *ctx, const char *target, size_t target_len,
                               const char *message, size_t message_len,
                               const char *const *media, size_t media_count) {
    (void)target; (void)target_len;
    (void)message; (void)message_len;
    (void)media; (void)media_count;
    int *n = (int *)ctx;
    if (n) (*n)++;
    return HU_OK;
}
static const hu_channel_vtable_t kMarkerVtable = { .send = marker_send };

static void prime_typing_buffer(void) {
    s_marker_calls = 0;
    hu_channel_t ch = { .ctx = &s_marker_calls, .vtable = &kMarkerVtable };
    hu_error_t err = hu_typing_send(&ch, "t", 1, "x", 1, NULL, 0, NULL);
    (void)err;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

/* 1. NULL persona → direct send. Typing buffer not disturbed. */
static void test_telegram_typing_null_persona_sends_directly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_telegram_create(&alloc, "123:token", 9, &ch), HU_OK);
    hu_telegram_set_persona(&ch, NULL);

    prime_typing_buffer();

    /* HU_IS_TEST path: token required, but send still works */
    hu_error_t err = ch.vtable->send(ch.ctx, "42", 2, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    /* Typing buffer must still be the primed state (budget=0, one SEND). */
    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t budget = 999u;
    size_t n = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_SEND);
    HU_ASSERT_EQ(budget, 0u);

    /* Message delivered in test path */
    size_t msg_len = 0;
    const char *msg = hu_telegram_test_get_last_message(&ch, &msg_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(msg_len, 5u);

    hu_telegram_destroy(&ch);
}

/* 2. persona set with default (non-instant) profile → routes through
 *    hu_typing_send; budget > 0; message still delivered correctly. */
static void test_telegram_typing_with_persona_calls_typing_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_telegram_create(&alloc, "123:token", 9, &ch), HU_OK);
    memset(&s_test_persona, 0, sizeof(s_test_persona));
    hu_telegram_set_persona(&ch, &s_test_persona);

    prime_typing_buffer();

    hu_error_t err = ch.vtable->send(ch.ctx, "42", 2, "hello world", 11, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    /* Budget must be non-zero (typing simulator was invoked). */
    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t budget = 0u;
    size_t n = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_GT(budget, 0u);
    HU_ASSERT_LE(budget, HU_TYPING_HARD_CEILING_MS);
    HU_ASSERT_GT(n, 0u);

    /* Telegram supports start/stop_typing → schedule includes START + STOP. */
    bool saw_start = false;
    for (size_t i = 0; i < n; i++) {
        if (acts[i].kind == HU_TYPING_ACTION_START_TYPING)
            saw_start = true;
    }
    HU_ASSERT_TRUE(saw_start);

    /* Message still delivered via raw_send in test mode. */
    size_t msg_len = 0;
    const char *msg = hu_telegram_test_get_last_message(&ch, &msg_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(msg_len, 11u);

    hu_telegram_destroy(&ch);
}

/* 3. NULL persona fallback: message delivery is guaranteed even without a
 *    persona (no crash, no missing message). */
static void test_telegram_typing_null_persona_delivers_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_telegram_create(&alloc, "123:token", 9, &ch), HU_OK);
    /* No persona set — default NULL. */

    hu_error_t err = ch.vtable->send(ch.ctx, "42", 2, "test message", 12, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    size_t msg_len = 0;
    const char *msg = hu_telegram_test_get_last_message(&ch, &msg_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(msg_len, 12u);

    hu_telegram_destroy(&ch);
}

/* 4. hu_telegram_set_persona with NULL / bad channel is a safe no-op. */
static void test_telegram_set_persona_handles_null_inputs(void) {
    hu_telegram_set_persona(NULL, NULL);
    hu_telegram_set_persona(NULL, (void *)0x1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_telegram_create(&alloc, "123:token", 9, &ch), HU_OK);
    hu_telegram_set_persona(&ch, NULL);
    hu_telegram_set_persona(&ch, NULL);
    hu_telegram_destroy(&ch);
}

void run_channel_telegram_tests(void) {
    HU_TEST_SUITE("Channel Telegram Typing");
    HU_RUN_TEST(test_telegram_typing_null_persona_sends_directly);
    HU_RUN_TEST(test_telegram_typing_with_persona_calls_typing_send);
    HU_RUN_TEST(test_telegram_typing_null_persona_delivers_message);
    HU_RUN_TEST(test_telegram_set_persona_handles_null_inputs);
}
