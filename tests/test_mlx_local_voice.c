#include "human/core/allocator.h"
#include "human/voice/mlx_local.h"
#include "human/voice/provider.h"
#include "human/voice/realtime.h"
#include "test_framework.h"
#include <string.h>

/* MLX Local Voice Provider tests. */

static void mlx_local_create_connect_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_mlx_local_config_t cfg = {
        .endpoint = "http://127.0.0.1:8741",
        .model = "gemma-4-e4b-it-4bit",
        .system_prompt = "You are a helpful assistant.",
        .max_tokens = 128,
        .temperature = 0.7f,
    };
    hu_voice_provider_t p = {0};
    hu_error_t err = hu_voice_provider_mlx_local_create(&alloc, &cfg, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_NOT_NULL(p.ctx);

    err = p.vtable->connect(p.ctx);
    HU_ASSERT_EQ(err, HU_OK);

    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_t p = {0};
    hu_voice_provider_mlx_local_create(&alloc, NULL, &p);
    const char *name = p.vtable->get_name(p.ctx);
    HU_ASSERT_STR_EQ(name, "mlx_local");
    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_send_audio_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_t p = {0};
    hu_voice_provider_mlx_local_create(&alloc, NULL, &p);
    p.vtable->connect(p.ctx);
    unsigned char pcm[] = {0, 1, 2, 3};
    hu_error_t err = p.vtable->send_audio(p.ctx, pcm, sizeof(pcm));
    HU_ASSERT_EQ(err, HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_recv_event_mock_transcript(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_t p = {0};
    hu_voice_provider_mlx_local_create(&alloc, NULL, &p);
    p.vtable->connect(p.ctx);

    hu_voice_rt_event_t ev = {0};
    hu_error_t err = p.vtable->recv_event(p.ctx, &alloc, &ev, 10);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ev.type, "response.text.delta");
    HU_ASSERT_NOT_NULL(ev.transcript);
    HU_ASSERT(ev.transcript_len > 0);
    hu_voice_rt_event_free(&alloc, &ev);

    memset(&ev, 0, sizeof(ev));
    err = p.vtable->recv_event(p.ctx, &alloc, &ev, 10);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ev.type, "response.done");
    HU_ASSERT(ev.done);
    HU_ASSERT(ev.generation_complete);
    hu_voice_rt_event_free(&alloc, &ev);

    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_cancel_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_t p = {0};
    hu_voice_provider_mlx_local_create(&alloc, NULL, &p);
    p.vtable->connect(p.ctx);
    HU_ASSERT_EQ(p.vtable->cancel_response(p.ctx), HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_activity_signals(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_t p = {0};
    hu_voice_provider_mlx_local_create(&alloc, NULL, &p);
    p.vtable->connect(p.ctx);
    HU_ASSERT_EQ(p.vtable->send_activity_start(p.ctx), HU_OK);
    HU_ASSERT_EQ(p.vtable->send_activity_end(p.ctx), HU_OK);
    HU_ASSERT_EQ(p.vtable->send_audio_stream_end(p.ctx), HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_reconnect(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_t p = {0};
    hu_voice_provider_mlx_local_create(&alloc, NULL, &p);
    p.vtable->connect(p.ctx);
    HU_ASSERT_EQ(p.vtable->reconnect(p.ctx), HU_OK);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_null_args_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_voice_provider_mlx_local_create(NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_voice_provider_t p = {0};
    err = hu_voice_provider_mlx_local_create(NULL, NULL, &p);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void mlx_local_default_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_t p = {0};
    hu_error_t err = hu_voice_provider_mlx_local_create(&alloc, NULL, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable);
    p.vtable->disconnect(p.ctx, &alloc);
}

static void mlx_local_factory_integration(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_provider_extras_t extras = {
        .system_instruction = "Test system prompt",
        .model_id = "gemma-4-e4b-it-4bit",
        .voice_id = "test-voice",
    };
    hu_voice_provider_t vp = {0};
    hu_error_t err = hu_voice_provider_create_from_extras(&alloc, "mlx_local", &extras, &vp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(vp.vtable);
    HU_ASSERT_STR_EQ(vp.vtable->get_name(vp.ctx), "mlx_local");

    err = vp.vtable->connect(vp.ctx);
    HU_ASSERT_EQ(err, HU_OK);

    hu_voice_rt_event_t ev = {0};
    err = vp.vtable->recv_event(vp.ctx, &alloc, &ev, 10);
    HU_ASSERT_EQ(err, HU_OK);
    hu_voice_rt_event_free(&alloc, &ev);

    vp.vtable->disconnect(vp.ctx, &alloc);
}

void run_mlx_local_voice_tests(void) {
    HU_TEST_SUITE("MLX Local Voice");
    HU_RUN_TEST(mlx_local_create_connect_destroy);
    HU_RUN_TEST(mlx_local_get_name);
    HU_RUN_TEST(mlx_local_send_audio_noop);
    HU_RUN_TEST(mlx_local_recv_event_mock_transcript);
    HU_RUN_TEST(mlx_local_cancel_response);
    HU_RUN_TEST(mlx_local_activity_signals);
    HU_RUN_TEST(mlx_local_reconnect);
    HU_RUN_TEST(mlx_local_null_args_rejected);
    HU_RUN_TEST(mlx_local_default_config);
    HU_RUN_TEST(mlx_local_factory_integration);
}
