/* tests/test_daemon_rich_media.c
 *
 * hu_daemon_rich_media_tick — music teaser / sticker / rich-link block carved
 * out of hu_service_run (src/daemon/daemon_rich_media.c, 2026-09-12). The
 * block is one guarded `if`: it must not touch the channel when the turn has
 * no text, or when a GIF already went out this turn. Both are real contracts
 * the service loop relies on (a second rich send after a GIF is the double-
 * bubble tell); a mock channel counts sends so a regression that sends fails
 * here, not on a contact's phone. */
#include "human/agent.h"
#include "human/daemon.h"
#include "test_framework.h"

#include <string.h>

static int g_sends;

static const char *rm_name(void *ctx) {
    (void)ctx;
    return "imessage";
}

static hu_error_t rm_send(void *ctx, const char *to, size_t to_len, const char *text,
                          size_t text_len, const char *const *attachments,
                          size_t attachment_count) {
    (void)ctx;
    (void)to;
    (void)to_len;
    (void)text;
    (void)text_len;
    (void)attachments;
    (void)attachment_count;
    g_sends++;
    return HU_OK;
}

static void test_rich_media_no_text_sends_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.name = rm_name;
    vt.send = rm_send;
    hu_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.vtable = &vt;
    hu_service_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.channel = &channel;

    g_sends = 0;
    hu_daemon_rich_media_tick(&alloc, &agent, NULL, &ch, "+15550000001", 12, "", 0, NULL, 0, false);
    HU_ASSERT_EQ(g_sends, 0);
}

static void test_rich_media_after_gif_sends_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.name = rm_name;
    vt.send = rm_send;
    hu_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.vtable = &vt;
    hu_service_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.channel = &channel;

    static const char msg[] = "omg you have to hear this song it's been stuck in my head all day";
    g_sends = 0;
    hu_daemon_rich_media_tick(&alloc, &agent, NULL, &ch, "+15550000001", 12, msg, sizeof(msg) - 1,
                              NULL, 0, /* gif_sent_this_turn */ true);
    HU_ASSERT_EQ(g_sends, 0);
}

void run_daemon_rich_media_tests(void) {
    HU_TEST_SUITE("daemon rich media (carved)");
    HU_RUN_TEST(test_rich_media_no_text_sends_nothing);
    HU_RUN_TEST(test_rich_media_after_gif_sends_nothing);
}
