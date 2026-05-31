#include "human/core/allocator.h"
#include "human/voice/local_stt.h"
#include "human/voice/local_tts.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void local_stt_transcribe_mock_returns_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_stt_config_t cfg = {.endpoint = "http://localhost:8000/v1/audio/transcriptions"};
    char *txt = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_local_stt_transcribe(&alloc, &cfg, "/tmp/fake.wav", &txt, &len), HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT_STR_EQ(txt, "Hello world");
    HU_ASSERT_EQ(len, strlen("Hello world"));
    alloc.free(alloc.ctx, txt, len + 1);
}

static void local_stt_transcribe_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *txt = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_local_stt_transcribe(NULL, NULL, "x.wav", &txt, &len), HU_ERR_INVALID_ARGUMENT);
    hu_local_stt_config_t cfg = {.endpoint = "http://localhost/x"};
    HU_ASSERT_EQ(hu_local_stt_transcribe(&alloc, &cfg, NULL, &txt, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_local_stt_transcribe(&alloc, &cfg, "", &txt, &len), HU_ERR_INVALID_ARGUMENT);
}

/* Does the built argv contain `needle` as an exact entry? */
static bool stt_argv_has(const hu_local_stt_request_t *req, const char *needle) {
    for (size_t i = 0; i < req->argc; i++) {
        if (req->argv[i] && strcmp(req->argv[i], needle) == 0)
            return true;
    }
    return false;
}

/* whisper.cpp /inference: file=@ + response_format=json, NO model/language
 * unless configured (the server loads its model at startup). */
static void local_stt_build_request_whispercpp_default_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_stt_config_t cfg = {.endpoint = "http://127.0.0.1:8080/inference"};
    hu_local_stt_request_t req;
    HU_ASSERT_EQ(hu_local_stt_build_request(&alloc, &cfg, "/tmp/clip.wav", &req), HU_OK);

    HU_ASSERT_STR_EQ(req.argv[0], "curl");
    HU_ASSERT_TRUE(stt_argv_has(&req, "file=@/tmp/clip.wav"));
    HU_ASSERT_TRUE(stt_argv_has(&req, "response_format=json"));
    HU_ASSERT_FALSE(stt_argv_has(&req, "model=whisper-large-v3"));
    /* endpoint is the final non-NULL entry; argv is NULL-terminated at argc */
    HU_ASSERT_TRUE(req.argc >= 1);
    HU_ASSERT_STR_EQ(req.argv[req.argc - 1], "http://127.0.0.1:8080/inference");
    HU_ASSERT_NULL(req.argv[req.argc]);

    hu_local_stt_request_free(&alloc, &req);
}

/* OpenAI-compatible server: model + language emitted when set. */
static void local_stt_build_request_includes_model_and_language_when_set(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_stt_config_t cfg = {.endpoint = "http://localhost:8000/v1/audio/transcriptions",
                                 .model = "whisper-large-v3",
                                 .language = "en"};
    hu_local_stt_request_t req;
    HU_ASSERT_EQ(hu_local_stt_build_request(&alloc, &cfg, "/tmp/clip.wav", &req), HU_OK);

    HU_ASSERT_TRUE(stt_argv_has(&req, "file=@/tmp/clip.wav"));
    HU_ASSERT_TRUE(stt_argv_has(&req, "response_format=json"));
    HU_ASSERT_TRUE(stt_argv_has(&req, "model=whisper-large-v3"));
    HU_ASSERT_TRUE(stt_argv_has(&req, "language=en"));
    HU_ASSERT_STR_EQ(req.argv[req.argc - 1], "http://localhost:8000/v1/audio/transcriptions");

    hu_local_stt_request_free(&alloc, &req);
}

static void local_stt_build_request_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_stt_request_t req;
    HU_ASSERT_EQ(hu_local_stt_build_request(NULL, NULL, "x.wav", &req), HU_ERR_INVALID_ARGUMENT);
    hu_local_stt_config_t no_ep = {.endpoint = NULL};
    HU_ASSERT_EQ(hu_local_stt_build_request(&alloc, &no_ep, "x.wav", &req),
                 HU_ERR_INVALID_ARGUMENT);
    hu_local_stt_config_t cfg = {.endpoint = "http://127.0.0.1:8080/inference"};
    HU_ASSERT_EQ(hu_local_stt_build_request(&alloc, &cfg, NULL, &req), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_local_stt_build_request(&alloc, &cfg, "", &req), HU_ERR_INVALID_ARGUMENT);
}

static void local_tts_build_body_input_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_tts_config_t cfg = {.endpoint = "http://127.0.0.1:8880/v1/audio/speech"};
    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_local_tts_build_body(&alloc, &cfg, "hello", &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_STR_EQ(json, "{\"input\":\"hello\"}");
    HU_ASSERT_EQ(len, strlen("{\"input\":\"hello\"}"));
    alloc.free(alloc.ctx, json, len + 1);
}

/* Regression: model/voice must be single-quoted JSON. The prior inline builder
 * produced malformed {"model":""kokoro"",...} whenever a voice was configured —
 * which Kokoro requires, so every turnkey request was rejected by the server. */
static void local_tts_build_body_model_and_voice_well_formed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_tts_config_t cfg = {.endpoint = "http://127.0.0.1:8880/v1/audio/speech",
                                 .model = "kokoro",
                                 .voice = "af_heart"};
    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_local_tts_build_body(&alloc, &cfg, "hello", &json, &len), HU_OK);
    HU_ASSERT_STR_EQ(json, "{\"model\":\"kokoro\",\"voice\":\"af_heart\",\"input\":\"hello\"}");
    alloc.free(alloc.ctx, json, len + 1);
}

static void local_tts_build_body_escapes_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_tts_config_t cfg = {.endpoint = "http://127.0.0.1:8880/v1/audio/speech"};
    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_local_tts_build_body(&alloc, &cfg, "he said \"hi\"", &json, &len), HU_OK);
    HU_ASSERT_STR_EQ(json, "{\"input\":\"he said \\\"hi\\\"\"}");
    alloc.free(alloc.ctx, json, len + 1);
}

static void local_tts_build_body_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    size_t len = 0;
    hu_local_tts_config_t cfg = {.endpoint = "http://x"};
    HU_ASSERT_EQ(hu_local_tts_build_body(NULL, NULL, "hi", &json, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_local_tts_build_body(&alloc, &cfg, NULL, &json, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_local_tts_build_body(&alloc, &cfg, "", &json, &len), HU_ERR_INVALID_ARGUMENT);
}

static void local_tts_synthesize_mock_returns_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_local_tts_config_t cfg = {.endpoint = "http://localhost:8880/v1/audio/speech"};
    char *path = NULL;
    HU_ASSERT_EQ(hu_local_tts_synthesize(&alloc, &cfg, "hello", &path), HU_OK);
    HU_ASSERT_NOT_NULL(path);
    HU_ASSERT(strlen(path) > 0);
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
    (void)remove(path);
    alloc.free(alloc.ctx, path, strlen(path) + 1);
}

static void local_tts_synthesize_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *path = NULL;
    HU_ASSERT_EQ(hu_local_tts_synthesize(NULL, NULL, "hi", &path), HU_ERR_INVALID_ARGUMENT);
    hu_local_tts_config_t cfg = {.endpoint = "http://x"};
    HU_ASSERT_EQ(hu_local_tts_synthesize(&alloc, &cfg, NULL, &path), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_local_tts_synthesize(&alloc, &cfg, "", &path), HU_ERR_INVALID_ARGUMENT);
}

void run_local_voice_tests(void) {
    HU_TEST_SUITE("LocalVoice");
    HU_RUN_TEST(local_stt_transcribe_mock_returns_text);
    HU_RUN_TEST(local_stt_transcribe_null_args_returns_error);
    HU_RUN_TEST(local_stt_build_request_whispercpp_default_fields);
    HU_RUN_TEST(local_stt_build_request_includes_model_and_language_when_set);
    HU_RUN_TEST(local_stt_build_request_null_args_returns_error);
    HU_RUN_TEST(local_tts_build_body_input_only);
    HU_RUN_TEST(local_tts_build_body_model_and_voice_well_formed);
    HU_RUN_TEST(local_tts_build_body_escapes_input);
    HU_RUN_TEST(local_tts_build_body_null_args_returns_error);
    HU_RUN_TEST(local_tts_synthesize_mock_returns_path);
    HU_RUN_TEST(local_tts_synthesize_null_args_returns_error);
}
