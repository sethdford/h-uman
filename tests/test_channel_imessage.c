/* G3 regression tests: iMessage channel ↔ hu_typing_send wiring.
 *
 * Mirrors the pattern from tests/test_channel_cli.c.  Pins:
 *
 *   1. persona == NULL            ⇒ direct send (typing buffer untouched)
 *   2. persona set, non-instant   ⇒ hu_typing_send called, budget > 0,
 *                                    message still delivered
 *   3. native typing support      ⇒ schedule has START_TYPING actions
 *   4. hu_imessage_set_persona(NULL) is a safe no-op
 *
 * Note: iMessage typing tiers (IMCore / AX / AppleScript) are all no-ops
 * in HU_IS_TEST builds, so these tests run without any OS interactions.
 */

#include "human/agent/typing_simulator.h"
#include "human/channel.h"
#include "human/channels/imessage.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

static hu_persona_t s_test_persona;

static int s_marker_calls;
static hu_error_t marker_send(void *ctx, const char *t, size_t tl,
                               const char *m, size_t ml,
                               const char *const *med, size_t mc) {
    (void)t; (void)tl; (void)m; (void)ml; (void)med; (void)mc;
    int *n = (int *)ctx;
    if (n) (*n)++;
    return HU_OK;
}
static const hu_channel_vtable_t kMarkerVtable = { .send = marker_send };

static void prime_typing_buffer(void) {
    s_marker_calls = 0;
    hu_channel_t ch = { .ctx = &s_marker_calls, .vtable = &kMarkerVtable };
    (void)hu_typing_send(&ch, "t", 1, "x", 1, NULL, 0, NULL);
}

/* 1. NULL persona → direct send. Typing buffer not disturbed. */
static void test_imessage_typing_null_persona_sends_directly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15550001234", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_persona(&ch, NULL);

    prime_typing_buffer();

    /* In HU_IS_TEST, imessage_raw_send requires message_len > 0 or media. */
    hu_error_t err = ch.vtable->send(ch.ctx, "+15550001234", 12, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t budget = 999u;
    size_t n = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)acts[0].kind, (int)HU_TYPING_ACTION_SEND);
    HU_ASSERT_EQ(budget, 0u);

    size_t msg_len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &msg_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(msg_len, 5u);

    hu_imessage_destroy(&ch);
}

/* 2. persona set → routes through hu_typing_send; budget > 0. */
static void test_imessage_typing_with_persona_calls_typing_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15550001234", 12, NULL, 0, &ch), HU_OK);
    memset(&s_test_persona, 0, sizeof(s_test_persona));
    hu_imessage_set_persona(&ch, &s_test_persona);

    prime_typing_buffer();

    hu_error_t err = ch.vtable->send(ch.ctx, "+15550001234", 12, "hello world", 11, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_typing_action_t acts[HU_TYPING_SCHEDULE_CAP];
    uint32_t budget = 0u;
    size_t n = hu_typing_test_take_last(acts, HU_TYPING_SCHEDULE_CAP, &budget);
    HU_ASSERT_GT(budget, 0u);
    HU_ASSERT_LE(budget, HU_TYPING_HARD_CEILING_MS);
    HU_ASSERT_GT(n, 0u);

    /* iMessage supports start/stop_typing → schedule has START_TYPING. */
    bool saw_start = false;
    for (size_t i = 0; i < n; i++) {
        if (acts[i].kind == HU_TYPING_ACTION_START_TYPING)
            saw_start = true;
    }
    HU_ASSERT_TRUE(saw_start);

    size_t msg_len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &msg_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(msg_len, 11u);

    hu_imessage_destroy(&ch);
}

/* 3. NULL persona fallback: message delivered without crash. */
static void test_imessage_typing_null_persona_delivers_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15550001234", 12, NULL, 0, &ch), HU_OK);

    hu_error_t err = ch.vtable->send(ch.ctx, "+15550001234", 12, "test message", 12, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    size_t msg_len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &msg_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(msg_len, 12u);

    hu_imessage_destroy(&ch);
}

/* 4. hu_imessage_set_persona with NULL / bad channel is a safe no-op. */
static void test_imessage_set_persona_handles_null_inputs(void) {
    hu_imessage_set_persona(NULL, NULL);
    hu_imessage_set_persona(NULL, (void *)0x1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t   ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15550001234", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_persona(&ch, NULL);
    hu_imessage_set_persona(&ch, NULL);
    hu_imessage_destroy(&ch);
}

void run_channel_imessage_tests(void) {
    HU_TEST_SUITE("Channel iMessage Typing");
    HU_RUN_TEST(test_imessage_typing_null_persona_sends_directly);
    HU_RUN_TEST(test_imessage_typing_with_persona_calls_typing_send);
    HU_RUN_TEST(test_imessage_typing_null_persona_delivers_message);
    HU_RUN_TEST(test_imessage_set_persona_handles_null_inputs);
}
