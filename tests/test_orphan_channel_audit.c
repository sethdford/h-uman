/* FIX 4 — Orphan channel audit.
 *
 * Channels listed in `docs/orphan-channels.md` carry a STATUS comment in
 * their .c file but are not wired into `bootstrap.c`. This audit asserts:
 *
 *   1. Each orphan's create function still exists and successfully creates a
 *      channel object (i.e. the file isn't silently broken).
 *   2. Each orphan still reports a recognizable name() through its vtable.
 *   3. Each orphan's destroy function still exists.
 *
 * If a channel graduates to production, REMOVE its block here and from the
 * orphan-channels.md table. If a channel gets deleted, REMOVE both as well.
 * A graduated/deleted channel that still appears in this audit will fail to
 * link (its create symbol will be missing), forcing the cleanup. */

#include "human/channel.h"
#include "human/channels/dispatch.h"
#include "human/channels/maixcam.h"
#include "human/channels/mattermost.h"
#include "human/channels/signal.h"
#include "human/channels/web.h"
#include "human/channels/webhook.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

/* CLI channel: header-less. Forward-declare its public symbols. */
extern hu_error_t hu_cli_create(hu_allocator_t *alloc, hu_channel_t *out);
extern void hu_cli_destroy(hu_channel_t *ch);

static void orphan_signal_creates_and_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_signal_create(&alloc, "http://localhost:8080", 21, "+15555550000", 12, &ch),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_TRUE(strcmp(ch.vtable->name(ch.ctx), "signal") == 0);
    hu_signal_destroy(&ch);
}

static void orphan_mattermost_creates_and_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_mattermost_create(&alloc, "http://localhost:8065", 21, "test-token", 10, &ch),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_TRUE(strcmp(ch.vtable->name(ch.ctx), "mattermost") == 0);
    hu_mattermost_destroy(&ch);
}

static void orphan_maixcam_creates_and_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_maixcam_create(&alloc, "localhost", 9, 8080, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_TRUE(strcmp(ch.vtable->name(ch.ctx), "maixcam") == 0);
    hu_maixcam_destroy(&ch);
}

static void orphan_web_creates_and_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_web_create(&alloc, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_TRUE(strcmp(ch.vtable->name(ch.ctx), "web") == 0);
    hu_web_destroy(&ch);
}

static void orphan_cli_creates_and_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_cli_create(&alloc, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_TRUE(strcmp(ch.vtable->name(ch.ctx), "cli") == 0);
    hu_cli_destroy(&ch);
}

static void orphan_dispatch_creates_and_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_dispatch_create(&alloc, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_TRUE(strcmp(ch.vtable->name(ch.ctx), "dispatch") == 0);
    hu_dispatch_destroy(&ch);
}

static void orphan_webhook_creates_and_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_webhook_channel_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = (char *)"webhook-test";
    cfg.callback_url = (char *)"http://localhost:9999/hook";
    cfg.message_field = (char *)"message";
    cfg.sender_field = (char *)"sender";
    cfg.max_message_len = 4096;
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_webhook_channel_create(&alloc, &cfg, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    /* The webhook facade reports its configured name; assert it matches the
     * value we passed in (rather than a fixed string). This proves the
     * channel facade is intact. */
    HU_ASSERT_TRUE(strcmp(ch.vtable->name(ch.ctx), "webhook-test") == 0);
    hu_webhook_channel_destroy(&ch, &alloc);
}

void run_orphan_channel_audit_tests(void) {
    HU_TEST_SUITE("OrphanChannelAudit");
    HU_RUN_TEST(orphan_signal_creates_and_names);
    HU_RUN_TEST(orphan_mattermost_creates_and_names);
    HU_RUN_TEST(orphan_maixcam_creates_and_names);
    HU_RUN_TEST(orphan_web_creates_and_names);
    HU_RUN_TEST(orphan_cli_creates_and_names);
    HU_RUN_TEST(orphan_dispatch_creates_and_names);
    HU_RUN_TEST(orphan_webhook_creates_and_names);
}
