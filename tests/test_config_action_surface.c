#include "human/config.h"
#include "test_framework.h"
#include <string.h>

static void defaults_match_spec(void) {
    hu_config_t c;
    hu_allocator_t a = hu_system_allocator();
    hu_error_t err = hu_config_load(&a, &c);
    HU_ASSERT_EQ(err, HU_OK);

#ifdef __APPLE__
    HU_ASSERT(c.channels.imessage.action_surface_v2.enabled);
#else
    HU_ASSERT(!c.channels.imessage.action_surface_v2.enabled);
#endif
    HU_ASSERT_EQ((int)(c.channels.imessage.action_surface_v2.thread_affinity_default * 100), 30);
    HU_ASSERT_EQ(c.channels.imessage.action_surface_v2.min_reply_delay_ms, 1500);
    HU_ASSERT_EQ(c.channels.imessage.action_surface_v2.reply_delay_variance_ms, 600);
    HU_ASSERT(c.channels.imessage.action_surface_v2.sticker_dir != NULL);
    HU_ASSERT(strstr(c.channels.imessage.action_surface_v2.sticker_dir, "stickers") != NULL);

    hu_config_deinit(&c);
}

static void json_override_takes_effect(void) {
    const char *json = "{\"channels\":{\"imessage\":{\"action_surface_v2\":{"
                       "\"enabled\":true,\"thread_affinity_default\":0.5,"
                       "\"min_reply_delay_ms\":2000,\"reply_delay_variance_ms\":800,"
                       "\"sticker_dir\":\"/tmp/stickers\"}}}}";
    hu_config_t c;
    hu_allocator_t a = hu_system_allocator();
    hu_error_t err = hu_config_load(&a, &c);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_config_parse_json(&c, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT(c.channels.imessage.action_surface_v2.enabled);
    HU_ASSERT_EQ((int)(c.channels.imessage.action_surface_v2.thread_affinity_default * 100), 50);
    HU_ASSERT_EQ(c.channels.imessage.action_surface_v2.min_reply_delay_ms, 2000);
    HU_ASSERT_EQ(c.channels.imessage.action_surface_v2.reply_delay_variance_ms, 800);
    HU_ASSERT_EQ(strcmp(c.channels.imessage.action_surface_v2.sticker_dir, "/tmp/stickers"), 0);

    hu_config_deinit(&c);
}

void run_config_action_surface_tests(void) {
    HU_TEST_SUITE("config_action_surface");
    HU_RUN_TEST(defaults_match_spec);
    HU_RUN_TEST(json_override_takes_effect);
}
