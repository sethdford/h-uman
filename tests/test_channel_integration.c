/* Integration tests for Teams, Twilio, Google Chat channels.
 * Tests full lifecycle: create -> is_configured -> send -> name -> deinit.
 * Uses HU_IS_TEST (test build) which mocks network calls.
 */
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

#if HU_HAS_TEAMS
#include "human/channels/teams.h"
#endif
#if HU_HAS_TWILIO
#include "human/channels/twilio.h"
#endif
#if HU_HAS_GOOGLE_CHAT
#include "human/channels/google_chat.h"
#endif

#if HU_HAS_TEAMS
static void test_teams_integration_full_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_teams_create(&alloc, "https://example.com/incoming", 27, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_teams_is_configured(&ch));
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "teams");
    err = ch.vtable->send(ch.ctx, NULL, 0, "test message", 12, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_teams_destroy(&ch);
}

static void test_teams_integration_empty_config_not_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_teams_create(&alloc, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(hu_teams_is_configured(&ch));
    err = ch.vtable->send(ch.ctx, NULL, 0, "test", 4, NULL, 0);
    HU_ASSERT(err == HU_ERR_CHANNEL_NOT_CONFIGURED);
    hu_teams_destroy(&ch);
}
#endif

#if HU_HAS_TWILIO
static void test_twilio_integration_full_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_twilio_create(&alloc, "ACXXXX", 6, "token", 5, "+15551234567", 12,
                                      "+15559876543", 12, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_twilio_is_configured(&ch));
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "twilio");
    err = ch.vtable->send(ch.ctx, NULL, 0, "test message", 12, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_twilio_destroy(&ch);
}

static void test_twilio_integration_empty_config_not_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_twilio_create(&alloc, NULL, 0, NULL, 0, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(hu_twilio_is_configured(&ch));
    err = ch.vtable->send(ch.ctx, NULL, 0, "test", 4, NULL, 0);
    HU_ASSERT(err == HU_ERR_CHANNEL_NOT_CONFIGURED);
    hu_twilio_destroy(&ch);
}
#endif

#if HU_HAS_GOOGLE_CHAT
static void test_google_chat_integration_full_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_google_chat_create(
        &alloc, "https://chat.googleapis.com/v1/spaces/abc/webhooks", 45, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_google_chat_is_configured(&ch));
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "google_chat");
    err = ch.vtable->send(ch.ctx, NULL, 0, "test message", 12, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_google_chat_destroy(&ch);
}

static void test_google_chat_integration_empty_config_not_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_google_chat_create(&alloc, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(hu_google_chat_is_configured(&ch));
    err = ch.vtable->send(ch.ctx, NULL, 0, "test", 4, NULL, 0);
    HU_ASSERT(err == HU_ERR_CHANNEL_NOT_CONFIGURED);
    hu_google_chat_destroy(&ch);
}
#endif

void run_channel_integration_tests(void) {
    HU_TEST_SUITE("Channel Integration");
#if HU_HAS_TEAMS
    HU_RUN_TEST(test_teams_integration_full_lifecycle);
    HU_RUN_TEST(test_teams_integration_empty_config_not_configured);
#endif
#if HU_HAS_TWILIO
    HU_RUN_TEST(test_twilio_integration_full_lifecycle);
    HU_RUN_TEST(test_twilio_integration_empty_config_not_configured);
#endif
#if HU_HAS_GOOGLE_CHAT
    HU_RUN_TEST(test_google_chat_integration_full_lifecycle);
    HU_RUN_TEST(test_google_chat_integration_empty_config_not_configured);
#endif
}
