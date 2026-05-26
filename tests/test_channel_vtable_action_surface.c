#include "test_framework.h"

#ifdef HU_HAS_DISCORD

#include "human/channel.h"
#include "human/channels/discord.h"
#include "human/core/allocator.h"

/* Test that the new vtable slots (reply/react_emoji/send_sticker) are wired to NULL
 * in non-iMessage channels. This is primarily a compile-gate — if the vtable extension
 * was incorrect, the build fails. The runtime assertions are defensive. */

static void vtable_has_reply_react_emoji_send_sticker_slots(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;

    /* Use Discord as a test channel (always available, simple to construct). */
    hu_error_t err = hu_discord_create(&alloc, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);

    /* The new slots should exist in the vtable struct. They should be NULL
     * for Discord (which doesn't implement these yet). */
    HU_ASSERT_NULL(ch.vtable->reply);
    HU_ASSERT_NULL(ch.vtable->react_emoji);
    HU_ASSERT_NULL(ch.vtable->send_sticker);

    hu_discord_destroy(&ch);
}

void run_channel_vtable_action_surface_tests(void) {
    HU_TEST_SUITE("channel_vtable_action_surface");
    HU_RUN_TEST(vtable_has_reply_react_emoji_send_sticker_slots);
}

#else /* !HU_HAS_DISCORD — stub runner. */
void run_channel_vtable_action_surface_tests(void) {
    (void)0;
}
#endif
