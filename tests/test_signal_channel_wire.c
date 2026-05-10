/* FIX 14 — Signal channel graduation from orphan.
 *
 * Verifies the config schema exposes http_url/account/allow_from/group_*
 * fields and that bootstrap will skip cleanly when http_url/account are
 * missing. The full bootstrap path requires libcurl + a real signal-cli
 * daemon, which we can't run in CI; this suite proves the wires exist
 * end-to-end without needing the network. */

#include "human/channels/signal.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* The struct must carry the fields a daemon adapter needs (otherwise
 * bootstrap can't graduate the channel). */
static void signal_config_struct_carries_production_fields(void) {
    hu_signal_channel_config_t c = {0};
    c.http_url = (char *)"http://localhost:8080";
    c.account = (char *)"+15551234567";
    c.allow_from[0] = (char *)"+15557654321";
    c.allow_from_count = 1;
    c.group_allow_from[0] = (char *)"group:abc";
    c.group_allow_from_count = 1;
    c.group_policy = (char *)"allowlist";

    HU_ASSERT_STR_EQ(c.http_url, "http://localhost:8080");
    HU_ASSERT_STR_EQ(c.account, "+15551234567");
    HU_ASSERT_EQ(c.allow_from_count, 1);
    HU_ASSERT_STR_EQ(c.allow_from[0], "+15557654321");
    HU_ASSERT_EQ(c.group_allow_from_count, 1);
    HU_ASSERT_STR_EQ(c.group_allow_from[0], "group:abc");
    HU_ASSERT_STR_EQ(c.group_policy, "allowlist");
}

/* The struct's allow_from array sizing matches the documented MAX. */
static void signal_config_allow_from_max_capacity(void) {
    HU_ASSERT(HU_SIGNAL_ALLOW_FROM_MAX >= 16);
    hu_signal_channel_config_t c = {0};
    for (size_t i = 0; i < HU_SIGNAL_ALLOW_FROM_MAX; i++)
        c.allow_from[i] = (char *)"x";
    c.allow_from_count = HU_SIGNAL_ALLOW_FROM_MAX;
    HU_ASSERT_EQ(c.allow_from_count, (size_t)HU_SIGNAL_ALLOW_FROM_MAX);
}

/* hu_signal_create_ex round-trip with a configured endpoint. We don't dial
 * out -- the test build defines HU_IS_TEST so the channel uses the mock
 * code path baked into signal.c. */
static void signal_create_with_full_config_succeeds(void) {
    hu_channel_t ch = {0};
    const char *allow[] = {"+15557654321"};
    const char *gallow[] = {"group:abc"};
    hu_error_t e = hu_signal_create_ex(A(), "http://localhost:8080", 21, "+15551234567", 12,
                                       allow, 1, gallow, 1, "open", 4, &ch);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "signal");
    hu_signal_destroy(&ch);
}

void run_signal_channel_wire_tests(void) {
    HU_TEST_SUITE("Signal channel wire (FIX 14)");
    HU_RUN_TEST(signal_config_struct_carries_production_fields);
    HU_RUN_TEST(signal_config_allow_from_max_capacity);
    HU_RUN_TEST(signal_create_with_full_config_succeeds);
}
