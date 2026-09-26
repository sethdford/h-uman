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
#include "human/channels/mattermost.h"
/* signal.h removed -- signal.c was graduated to production in FIX 14;
 * its production wires are tested in tests/test_signal_channel_wire.c. */
#include "human/channels/web.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

/* CLI channel: header-less. Forward-declare its public symbols. */
extern hu_error_t hu_cli_create(hu_allocator_t *alloc, hu_channel_t *out);
extern void hu_cli_destroy(hu_channel_t *ch);

/* signal removed -- graduated in FIX 14 (see test_signal_channel_wire.c). */

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

void run_orphan_channel_audit_tests(void) {
    HU_TEST_SUITE("OrphanChannelAudit");
    HU_RUN_TEST(orphan_mattermost_creates_and_names);
    HU_RUN_TEST(orphan_web_creates_and_names);
    HU_RUN_TEST(orphan_cli_creates_and_names);
    HU_RUN_TEST(orphan_dispatch_creates_and_names);
}
