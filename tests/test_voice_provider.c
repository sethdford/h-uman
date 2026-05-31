#include "human/core/allocator.h"
#include "human/core/privacy.h"
#include "human/voice/provider.h"
#include "human/voice/realtime.h"
#include "test_framework.h"
#include <string.h>

/* Voice Provider vtable abstraction tests. */

static void voice_provider_openai_create_connect_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    cfg.model = "test-model";
    cfg.sample_rate = 16000;
    hu_voice_provider_t p = {0};
    hu_error_t err = hu_voice_provider_openai_create(&alloc, &cfg, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_NOT_NULL(p.ctx);
    err = p.vtable->connect(p.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void voice_provider_openai_send_audio_dispatches(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    hu_voice_provider_t p = {0};
    hu_voice_provider_openai_create(&alloc, &cfg, &p);
    p.vtable->connect(p.ctx);
    unsigned char pcm[] = {0, 1, 2, 3};
    hu_error_t err = p.vtable->send_audio(p.ctx, pcm, sizeof(pcm));
    HU_ASSERT_EQ(err, HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void voice_provider_openai_recv_event_dispatches(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    hu_voice_provider_t p = {0};
    hu_voice_provider_openai_create(&alloc, &cfg, &p);
    p.vtable->connect(p.ctx);
    hu_voice_rt_event_t ev = {0};
    hu_error_t err = p.vtable->recv_event(p.ctx, &alloc, &ev, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_voice_rt_event_free(&alloc, &ev);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void voice_provider_openai_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    hu_voice_provider_t p = {0};
    hu_voice_provider_openai_create(&alloc, &cfg, &p);
    const char *name = p.vtable->get_name(p.ctx);
    HU_ASSERT_STR_EQ(name, "openai_realtime");
    p.vtable->disconnect(p.ctx, &alloc);
}

static void voice_provider_openai_create_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    hu_error_t err = hu_voice_provider_openai_create(&alloc, &cfg, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void voice_provider_openai_create_null_alloc_fails(void) {
    hu_voice_rt_config_t cfg = {0};
    hu_voice_provider_t p = {0};
    hu_error_t err = hu_voice_provider_openai_create(NULL, &cfg, &p);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void voice_provider_openai_cancel_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    hu_voice_provider_t p = {0};
    hu_voice_provider_openai_create(&alloc, &cfg, &p);
    p.vtable->connect(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable->cancel_response);
    HU_ASSERT_EQ(p.vtable->cancel_response(p.ctx), HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void voice_provider_openai_add_tool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    hu_voice_provider_t p = {0};
    hu_voice_provider_openai_create(&alloc, &cfg, &p);
    p.vtable->connect(p.ctx);
    hu_error_t err = p.vtable->add_tool(p.ctx, "search", "Search the web", "{}");
    HU_ASSERT_EQ(err, HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

/* Privacy kill-switch: OpenAI Realtime streams mic audio to the cloud, so the
 * create boundary must refuse it under privacy mode — this covers the direct
 * caller hu_voice_session_start (session.c), which bypasses the factory. */
static void voice_provider_openai_create_blocked_under_privacy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_rt_config_t cfg = {0};
    cfg.model = "test-model";
    hu_voice_provider_t p = {0};
    hu_privacy_set_enforced(true);
    hu_error_t err = hu_voice_provider_openai_create(&alloc, &cfg, &p);
    hu_privacy_set_enforced(false);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(p.vtable);
}

/* The factory refuses cloud realtime backends under privacy mode... */
static void voice_provider_factory_blocks_cloud_realtime_under_privacy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_extras_t extras = {0};
    extras.api_key = "sk-test";
    hu_voice_provider_t p = {0};
    hu_privacy_set_enforced(true);
    HU_ASSERT_EQ(hu_voice_provider_create_from_extras(&alloc, "openai_realtime", &extras, &p),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(p.vtable);
    HU_ASSERT_EQ(hu_voice_provider_create_from_extras(&alloc, "gemini_live", &extras, &p),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(p.vtable);
    hu_privacy_set_enforced(false);
}

/* ...but the on-device mlx_local backend stays available under privacy mode
 * (it never leaves the device), so privacy doesn't over-block local voice. */
static void voice_provider_factory_allows_local_under_privacy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_extras_t extras = {0};
    extras.model_id = "test-model";
    extras.voice_id = "test-voice";
    hu_voice_provider_t p = {0};
    hu_privacy_set_enforced(true);
    hu_error_t err = hu_voice_provider_create_from_extras(&alloc, "mlx_local", &extras, &p);
    hu_privacy_set_enforced(false);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable);
    if (p.vtable && p.vtable->disconnect)
        p.vtable->disconnect(p.ctx, &alloc);
}

void run_voice_provider_tests(void) {
    HU_TEST_SUITE("voice_provider");
    HU_RUN_TEST(voice_provider_openai_create_blocked_under_privacy);
    HU_RUN_TEST(voice_provider_factory_blocks_cloud_realtime_under_privacy);
    HU_RUN_TEST(voice_provider_factory_allows_local_under_privacy);
    HU_RUN_TEST(voice_provider_openai_create_connect_destroy);
    HU_RUN_TEST(voice_provider_openai_send_audio_dispatches);
    HU_RUN_TEST(voice_provider_openai_recv_event_dispatches);
    HU_RUN_TEST(voice_provider_openai_get_name);
    HU_RUN_TEST(voice_provider_openai_create_null_out_fails);
    HU_RUN_TEST(voice_provider_openai_create_null_alloc_fails);
    HU_RUN_TEST(voice_provider_openai_cancel_response);
    HU_RUN_TEST(voice_provider_openai_add_tool);
}
