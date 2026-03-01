#include "test_framework.h"
#include "seaclaw/voice.h"
#include "seaclaw/core/allocator.h"

static void test_voice_stt_file_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = "test-key", .api_key_len = 8 };
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_file(&alloc, &cfg, "/tmp/test.ogg", &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    SC_ASSERT(len > 0);
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_stt_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = "test-key", .api_key_len = 8 };
    char audio[] = {0x00, 0x01, 0x02};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt(&alloc, &cfg, audio, 3, &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_tts_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = "test-key", .api_key_len = 8 };
    void *audio = NULL;
    size_t audio_len = 0;
    sc_error_t err = sc_voice_tts(&alloc, &cfg, "hello world", 11, &audio, &audio_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(audio);
    SC_ASSERT(audio_len > 0);
    alloc.free(alloc.ctx, audio, audio_len);
}

static void test_voice_stt_no_api_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = NULL, .api_key_len = 0 };
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_file(&alloc, &cfg, "/tmp/test.ogg", &text, &len);
    SC_ASSERT(err != SC_OK);
}

static void test_voice_config_defaults(void) {
    sc_voice_config_t cfg = {0};
    SC_ASSERT(cfg.stt_endpoint == NULL);
    SC_ASSERT(cfg.tts_endpoint == NULL);
    SC_ASSERT(cfg.stt_model == NULL);
}

static void test_voice_stt_null_audio_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = "test-key", .api_key_len = 8 };
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt(&alloc, &cfg, NULL, 10, &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(text);
}

static void test_voice_stt_zero_length_ok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = "test-key", .api_key_len = 8 };
    char empty[] = "";
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt(&alloc, &cfg, empty, 0, &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_tts_zero_length_input_ok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = "test-key", .api_key_len = 8 };
    void *audio = NULL;
    size_t audio_len = 0;
    sc_error_t err = sc_voice_tts(&alloc, &cfg, "", 0, &audio, &audio_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(audio);
    alloc.free(alloc.ctx, audio, audio_len);
}

static void test_voice_stt_null_config_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char audio[] = {0x00, 0x01};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt(&alloc, NULL, audio, 2, &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_tts_null_config_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    void *audio = NULL;
    size_t audio_len = 0;
    sc_error_t err = sc_voice_tts(&alloc, NULL, "hello", 5, &audio, &audio_len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_stt_file_unsupported_format_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = { .api_key = "test-key", .api_key_len = 8 };
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_file(&alloc, &cfg, "/tmp/test.unknown_format", &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    SC_ASSERT(len > 0);
    alloc.free(alloc.ctx, text, len + 1);
}

void run_voice_tests(void) {
    SC_TEST_SUITE("Voice");
    SC_RUN_TEST(test_voice_stt_file_mock);
    SC_RUN_TEST(test_voice_stt_mock);
    SC_RUN_TEST(test_voice_tts_mock);
    SC_RUN_TEST(test_voice_stt_no_api_key);
    SC_RUN_TEST(test_voice_config_defaults);
    SC_RUN_TEST(test_voice_stt_null_audio_fails);
    SC_RUN_TEST(test_voice_stt_zero_length_ok);
    SC_RUN_TEST(test_voice_tts_zero_length_input_ok);
    SC_RUN_TEST(test_voice_stt_null_config_fails);
    SC_RUN_TEST(test_voice_tts_null_config_fails);
    SC_RUN_TEST(test_voice_stt_file_unsupported_format_mock);
}
