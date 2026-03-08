#include "seaclaw/core/allocator.h"
#include "seaclaw/voice.h"
#include "test_framework.h"

static void test_voice_stt_file_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
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
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
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
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
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
    sc_voice_config_t cfg = {.api_key = NULL, .api_key_len = 0};
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
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt(&alloc, &cfg, NULL, 10, &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(text);
}

static void test_voice_stt_zero_length_ok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
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
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
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

static void test_voice_play_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    unsigned char data[] = {0x00, 0x01, 0x02, 0x03};
    sc_error_t err = sc_voice_play(&alloc, data, 4);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_voice_play_null_audio(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_voice_play(&alloc, NULL, 10);
    SC_ASSERT(err != SC_OK);
}

static void test_voice_play_zero_length(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_voice_play(&alloc, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_voice_stt_file_unsupported_format_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_file(&alloc, &cfg, "/tmp/test.unknown_format", &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    SC_ASSERT(len > 0);
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_tts_null_alloc_fails(void) {
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    void *audio = NULL;
    size_t audio_len = 0;
    sc_error_t err = sc_voice_tts(NULL, &cfg, "hello", 5, &audio, &audio_len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_tts_large_input(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char large[1200];
    for (size_t i = 0; i < sizeof(large) - 1; i++)
        large[i] = 'a' + (char)(i % 26);
    large[sizeof(large) - 1] = '\0';
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    void *audio = NULL;
    size_t audio_len = 0;
    sc_error_t err = sc_voice_tts(&alloc, &cfg, large, sizeof(large) - 1, &audio, &audio_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(audio);
    alloc.free(alloc.ctx, audio, audio_len);
}

static void test_voice_stt_large_audio(void) {
    sc_allocator_t alloc = sc_system_allocator();
    unsigned char large[5000];
    for (size_t i = 0; i < sizeof(large); i++)
        large[i] = (unsigned char)(i & 0xff);
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt(&alloc, &cfg, large, sizeof(large), &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_play_large_data(void) {
    sc_allocator_t alloc = sc_system_allocator();
    unsigned char large[10240];
    for (size_t i = 0; i < sizeof(large); i++)
        large[i] = (unsigned char)(i & 0xff);
    sc_error_t err = sc_voice_play(&alloc, large, sizeof(large));
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_voice_stt_null_output_ptrs(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char audio[] = {0x00, 0x01};
    sc_error_t err = sc_voice_stt(&alloc, &cfg, audio, 2, NULL, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_tts_null_output_ptrs(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    sc_error_t err = sc_voice_tts(&alloc, &cfg, "hi", 2, NULL, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_config_with_custom_endpoints(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {
        .api_key = "test-key",
        .api_key_len = 8,
        .stt_endpoint = "https://example.com/stt",
        .tts_endpoint = "https://example.com/tts",
        .stt_model = "whisper-1",
        .tts_model = "tts-1-hd",
        .tts_voice = "nova",
        .language = "en",
    };
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_file(&alloc, &cfg, "/tmp/test.ogg", &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_stt_file_null_path_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_file(&alloc, &cfg, NULL, &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_stt_null_alloc_fails(void) {
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char audio[] = {0x00, 0x01};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt(NULL, &cfg, audio, 2, &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_play_null_alloc_fails(void) {
    unsigned char data[] = {0x00, 0x01};
    sc_error_t err = sc_voice_play(NULL, data, 2);
    SC_ASSERT_NEQ(err, SC_OK);
}

/* ── Gemini STT tests ──────────────────────────────────────────────── */

static void test_voice_stt_gemini_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_gemini(&alloc, &cfg, "dGVzdA==", 8, "audio/webm", &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
    SC_ASSERT(len > 0);
    SC_ASSERT_STR_EQ(text, "Mock Gemini transcription");
    alloc.free(alloc.ctx, text, len + 1);
}

static void test_voice_stt_gemini_null_audio_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_gemini(&alloc, &cfg, NULL, 0, "audio/webm", &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_stt_gemini_empty_audio_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_gemini(&alloc, &cfg, "", 0, "audio/webm", &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_stt_gemini_no_api_key_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = NULL, .api_key_len = 0};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_gemini(&alloc, &cfg, "dGVzdA==", 8, "audio/webm", &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_stt_gemini_null_config_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_gemini(&alloc, NULL, "dGVzdA==", 8, "audio/webm", &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_stt_gemini_null_alloc_fails(void) {
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_gemini(NULL, &cfg, "dGVzdA==", 8, "audio/webm", &text, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_stt_gemini_null_output_ptrs_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    sc_error_t err = sc_voice_stt_gemini(&alloc, &cfg, "dGVzdA==", 8, "audio/webm", NULL, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_voice_stt_gemini_null_mime_defaults_webm(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_voice_config_t cfg = {.api_key = "test-key", .api_key_len = 8};
    char *text = NULL;
    size_t len = 0;
    sc_error_t err = sc_voice_stt_gemini(&alloc, &cfg, "dGVzdA==", 8, NULL, &text, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(text);
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
    SC_RUN_TEST(test_voice_play_mock);
    SC_RUN_TEST(test_voice_play_null_audio);
    SC_RUN_TEST(test_voice_play_zero_length);
    SC_RUN_TEST(test_voice_tts_large_input);
    SC_RUN_TEST(test_voice_stt_large_audio);
    SC_RUN_TEST(test_voice_play_large_data);
    SC_RUN_TEST(test_voice_stt_null_output_ptrs);
    SC_RUN_TEST(test_voice_tts_null_output_ptrs);
    SC_RUN_TEST(test_voice_config_with_custom_endpoints);
    SC_RUN_TEST(test_voice_stt_file_null_path_mock);
    SC_RUN_TEST(test_voice_tts_null_alloc_fails);
    SC_RUN_TEST(test_voice_stt_null_alloc_fails);
    SC_RUN_TEST(test_voice_play_null_alloc_fails);
    SC_RUN_TEST(test_voice_stt_gemini_mock);
    SC_RUN_TEST(test_voice_stt_gemini_null_audio_fails);
    SC_RUN_TEST(test_voice_stt_gemini_empty_audio_fails);
    SC_RUN_TEST(test_voice_stt_gemini_no_api_key_fails);
    SC_RUN_TEST(test_voice_stt_gemini_null_config_fails);
    SC_RUN_TEST(test_voice_stt_gemini_null_alloc_fails);
    SC_RUN_TEST(test_voice_stt_gemini_null_output_ptrs_fails);
    SC_RUN_TEST(test_voice_stt_gemini_null_mime_defaults_webm);
}
