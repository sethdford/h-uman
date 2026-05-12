/*
 * Integration test for the send_voice_message → TTS → channel send pipeline.
 *
 * Exercises the exact sequence the daemon uses:
 *   1. Tool sets pending voice state (transcript + emotion)
 *   2. Pending voice detected → forced HU_VOICE_SEND_VOICE
 *   3. Nonverbal injection into transcript
 *   4. Emotion resolved (override or auto-detect)
 *   5. Cartesia TTS synthesize (HU_IS_TEST mock → 400 bytes)
 *   6. Audio pipeline writes temp file
 *   7. Channel send called with media path
 *   8. Temp file cleaned up
 *   9. Pending voice cleared
 *
 * This proves the full wiring without needing a Cartesia API key.
 */
#include "human/agent.h"
#include "human/agent/tool_context.h"
#include "human/config.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/tts/transcript_prep.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/persona.h"
#include "human/tools/send_voice_message.h"
#include "human/tts/audio_pipeline.h"
#include "human/tts/cartesia.h"
#include "human/tts/emotion_map.h"
#include "human/voice/emotion_voice_map.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static hu_allocator_t alloc;

static void setup(void) {
    alloc = hu_system_allocator();
}

/* ── Test: tool → pending state → TTS → audio file → cleanup ────────── */

static void test_full_pipeline_tool_to_audio_file(void) {
    setup();

    /* 1. Create tool and set agent context */
    hu_tool_t tool = {0};
    hu_send_voice_message_create(&alloc, &tool);

    hu_agent_t fake_agent = {0};
    hu_agent_set_current_for_tools(&fake_agent);
    hu_agent_clear_pending_voice();

    /* 2. Tool call: request voice with custom transcript and emotion */
    const char *json = "{\"transcript\":\"Hey, I just wanted to say I'm proud of you.\","
                       "\"emotion\":\"sympathetic\"}";
    hu_json_value_t *parsed = NULL;
    hu_json_parse(&alloc, json, strlen(json), &parsed);

    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, parsed, &result);
    HU_ASSERT_TRUE(result.success);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, parsed);

    /* 3. Verify pending voice state (daemon reads this) */
    HU_ASSERT_TRUE(hu_agent_has_pending_voice());
    size_t pv_len = 0;
    const char *pv_transcript = hu_agent_pending_voice_transcript(&pv_len);
    const char *pv_emotion = hu_agent_pending_voice_emotion();
    HU_ASSERT_NOT_NULL(pv_transcript);
    HU_ASSERT_TRUE(pv_len > 0);
    HU_ASSERT_STR_EQ(pv_emotion, "sympathetic");

    /* 4. Daemon path: build voice transcript (copy to stack buffer, inject nonverbals) */
    char voice_transcript[4096];
    size_t vt_len = pv_len < sizeof(voice_transcript) - 64 ? pv_len
                                                           : sizeof(voice_transcript) - 64;
    memcpy(voice_transcript, pv_transcript, vt_len);
    voice_transcript[vt_len] = '\0';
    vt_len = hu_conversation_inject_nonverbals(voice_transcript, vt_len,
                                               sizeof(voice_transcript), 42, true);
    HU_ASSERT_TRUE(vt_len > 0);

    /* 5. Emotion: use override from tool (daemon uses pv_emotion if set) */
    const char *emo_str = pv_emotion;
    HU_ASSERT_STR_EQ(emo_str, "sympathetic");

    /* 6. Cartesia TTS synthesize (mock under HU_IS_TEST) */
    hu_cartesia_tts_config_t tts_cfg = {
        .model_id = "sonic-3-2026-01-12",
        .voice_id = "test-voice-uuid",
        .emotion = emo_str,
        .speed = 0.95f,
        .volume = 1.0f,
        .nonverbals = true,
    };
    unsigned char *audio_bytes = NULL;
    size_t audio_len = 0;
    hu_error_t tts_err = hu_cartesia_tts_synthesize(
        &alloc, "test-key", 8, voice_transcript, vt_len, &tts_cfg, "mp3",
        &audio_bytes, &audio_len);

#if HU_ENABLE_CARTESIA
    HU_ASSERT_EQ(tts_err, HU_OK);
    HU_ASSERT_NOT_NULL(audio_bytes);
    HU_ASSERT_EQ(audio_len, 400u);
    HU_ASSERT_EQ(audio_bytes[0], (unsigned char)0xFF);
    HU_ASSERT_EQ(audio_bytes[1], (unsigned char)0xFB);

    /* 7. Audio pipeline: write to temp file */
    char audio_path[512];
    hu_error_t pipe_err = hu_audio_tts_bytes_to_temp(
        &alloc, audio_bytes, audio_len, "mp3", audio_path, sizeof(audio_path));
    HU_ASSERT_EQ(pipe_err, HU_OK);
    HU_ASSERT_TRUE(audio_path[0] != '\0');

    /* 8. Verify temp file exists (basic check) */
    FILE *f = fopen(audio_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fclose(f);
        HU_ASSERT_TRUE(fsize > 0);
    }

    /* 9. Clean up temp file (daemon calls this after channel send) */
    hu_audio_cleanup_temp(audio_path);

    /* Verify temp file removed */
    f = fopen(audio_path, "rb");
    HU_ASSERT_NULL(f);

    /* 10. Free TTS bytes */
    hu_cartesia_tts_free_bytes(&alloc, audio_bytes, audio_len);
#else
    (void)tts_err;
    /* Under HU_IS_TEST the mock at src/tts/cartesia.c:242-260 allocates
     * 400 b unconditionally regardless of HU_ENABLE_CARTESIA. The Linux
     * ASan CI compiles without Cartesia and was leaking those 400 b on
     * every run. Free on both paths. */
    if (audio_bytes)
        hu_cartesia_tts_free_bytes(&alloc, audio_bytes, audio_len);
#endif

    /* 11. Clear pending voice (daemon does this unconditionally) */
    hu_agent_clear_pending_voice();
    HU_ASSERT_FALSE(hu_agent_has_pending_voice());

    hu_agent_clear_current_for_tools();
}

/* ── Test: auto-detected emotion (no override) ──────────────────────── */

static void test_pipeline_auto_emotion_from_context(void) {
    setup();

    hu_tool_t tool = {0};
    hu_send_voice_message_create(&alloc, &tool);
    hu_agent_t fake_agent = {0};
    hu_agent_set_current_for_tools(&fake_agent);
    hu_agent_clear_pending_voice();

    /* Tool call with transcript only, no emotion */
    const char *json = "{\"transcript\":\"Congratulations on the promotion!\"}";
    hu_json_value_t *parsed = NULL;
    hu_json_parse(&alloc, json, strlen(json), &parsed);
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, parsed, &result);
    HU_ASSERT_TRUE(result.success);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, parsed);

    /* No emotion override → daemon uses hu_cartesia_emotion_from_context */
    const char *pv_emotion = hu_agent_pending_voice_emotion();
    HU_ASSERT_TRUE(pv_emotion == NULL || pv_emotion[0] == '\0');

    /* Simulate daemon: auto-detect emotion from response */
    const char *response = "Congratulations on the promotion!";
    size_t resp_len = strlen(response);
    const char *emo_str = hu_cartesia_emotion_from_context(
        "I got promoted!", 15, response, resp_len, 14);
#ifdef HU_ENABLE_CARTESIA
    HU_ASSERT_STR_EQ(emo_str, "excited");
#else
    HU_ASSERT_STR_EQ(emo_str, "content");
#endif

    /* Also test emotion-voice-map composition */
    hu_voice_emotion_t detected = HU_VOICE_EMOTION_NEUTRAL;
    float confidence = 0.0f;
    hu_emotion_detect_from_text(response, resp_len, &detected, &confidence);
    hu_voice_params_t params = hu_emotion_voice_map(detected);
    HU_ASSERT_TRUE(params.rate_factor > 0.0f);

    hu_agent_clear_pending_voice();
    hu_agent_clear_current_for_tools();
}

/* ── Test: response text as TTS source when no custom transcript ───── */

static void test_pipeline_response_as_tts_source(void) {
    setup();

    hu_tool_t tool = {0};
    hu_send_voice_message_create(&alloc, &tool);
    hu_agent_t fake_agent = {0};
    hu_agent_set_current_for_tools(&fake_agent);
    hu_agent_clear_pending_voice();

    /* Tool call with no arguments → response text will be spoken */
    hu_json_value_t args = {.type = HU_JSON_OBJECT};
    args.data.object.pairs = NULL;
    args.data.object.len = 0;
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, &args, &result);
    HU_ASSERT_TRUE(result.success);
    hu_tool_result_free(&alloc, &result);

    /* Pending voice set, but no custom transcript */
    HU_ASSERT_TRUE(hu_agent_has_pending_voice());
    size_t pv_len = 0;
    const char *pv_transcript = hu_agent_pending_voice_transcript(&pv_len);
    HU_ASSERT_TRUE(pv_transcript == NULL || pv_len == 0);

    /* Daemon would use response as tts_source */
    const char *response = "I'm so happy for you.";
    size_t response_len = strlen(response);
    const char *tts_source = response;
    size_t tts_source_len = response_len;
    if (pv_transcript && pv_len > 0) {
        tts_source = pv_transcript;
        tts_source_len = pv_len;
    }
    HU_ASSERT_TRUE(tts_source == response);
    HU_ASSERT_EQ(tts_source_len, response_len);

    hu_agent_clear_pending_voice();
    hu_agent_clear_current_for_tools();
}

/* ── Test: iMessage format selection ─────────────────────────────────── */

static void test_pipeline_imessage_selects_caf(void) {
    const char *fmt = hu_tts_format_for_channel("imessage");
    HU_ASSERT_STR_EQ(fmt, "caf");
}

static void test_pipeline_telegram_selects_ogg(void) {
    const char *fmt = hu_tts_format_for_channel("telegram");
    HU_ASSERT_STR_EQ(fmt, "ogg");
}

static void test_pipeline_slack_selects_mp3(void) {
    const char *fmt = hu_tts_format_for_channel("slack");
    HU_ASSERT_STR_EQ(fmt, "mp3");
}

/* ── Test: rate limit prevents double voice in same turn ─────────────── */

static void test_pipeline_rate_limit_single_voice_per_turn(void) {
    setup();

    hu_tool_t tool = {0};
    hu_send_voice_message_create(&alloc, &tool);
    hu_agent_t fake_agent = {0};
    hu_agent_set_current_for_tools(&fake_agent);
    hu_agent_clear_pending_voice();

    hu_json_value_t args = {.type = HU_JSON_OBJECT};
    args.data.object.pairs = NULL;
    args.data.object.len = 0;

    hu_tool_result_t r1 = {0};
    tool.vtable->execute(tool.ctx, &alloc, &args, &r1);
    HU_ASSERT_TRUE(r1.success);
    hu_tool_result_free(&alloc, &r1);

    /* Second call in same turn → rejected */
    hu_tool_result_t r2 = {0};
    tool.vtable->execute(tool.ctx, &alloc, &args, &r2);
    HU_ASSERT_FALSE(r2.success);
    HU_ASSERT_STR_CONTAINS(r2.error_msg, "already requested");

    /* Clear and verify can call again (simulates next turn) */
    hu_agent_clear_pending_voice();
    hu_tool_result_t r3 = {0};
    tool.vtable->execute(tool.ctx, &alloc, &args, &r3);
    HU_ASSERT_TRUE(r3.success);
    hu_tool_result_free(&alloc, &r3);

    hu_agent_clear_pending_voice();
    hu_agent_clear_current_for_tools();
}

/* ── Test: full pipeline with transcript preprocessor ─────────────────── */

static void test_pipeline_with_preprocessor(void) {
    setup();

    hu_tool_t tool = {0};
    hu_send_voice_message_create(&alloc, &tool);
    hu_agent_t fake_agent = {0};
    hu_agent_set_current_for_tools(&fake_agent);
    hu_agent_clear_pending_voice();

    /* Multi-sentence transcript with emotional range */
    const char *json =
        "{\"transcript\":\"I'm so sorry about what happened. "
        "But honestly, you handled it with so much grace. "
        "I'm really proud of you!\"}";
    hu_json_value_t *parsed = NULL;
    hu_json_parse(&alloc, json, strlen(json), &parsed);
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, parsed, &result);
    HU_ASSERT_TRUE(result.success);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, parsed);

    size_t pv_len = 0;
    const char *pv_transcript = hu_agent_pending_voice_transcript(&pv_len);
    HU_ASSERT_NOT_NULL(pv_transcript);
    HU_ASSERT_TRUE(pv_len > 0);

    /* Run preprocessor (the daemon path now does this) */
    hu_prep_config_t prep_cfg = {
        .incoming_msg = "I lost my job today",
        .incoming_msg_len = 19,
        .base_speed = 0.95f,
        .pause_factor = 1.2f,
        .discourse_rate = 0.3f,
        .nonverbals_enabled = true,
        .seed = 42,
        .hour_local = 14,
    };
    hu_prep_result_t prep = {0};
    hu_error_t err = hu_transcript_prep(pv_transcript, pv_len, &prep_cfg, &prep);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(prep.output_len > 0);
    HU_ASSERT_TRUE(prep.sentence_count >= 2);
    HU_ASSERT_NOT_NULL(prep.dominant_emotion);
    HU_ASSERT_TRUE(prep.volume > 0.0f && prep.volume <= 2.0f);

    /* Verify SSML annotations are present */
    HU_ASSERT_TRUE(strstr(prep.output, "<break time=") != NULL);
    HU_ASSERT_TRUE(strstr(prep.output, "<emotion value=") != NULL);

#if HU_ENABLE_CARTESIA
    /* Feed preprocessed output to TTS */
    hu_cartesia_tts_config_t tts_cfg = {
        .model_id = "sonic-3-2026-01-12",
        .voice_id = "test-voice-uuid",
        .emotion = prep.dominant_emotion,
        .speed = 0.95f,
        .volume = prep.volume,
        .nonverbals = true,
    };
    unsigned char *audio_bytes = NULL;
    size_t audio_len = 0;
    hu_error_t tts_err = hu_cartesia_tts_synthesize(
        &alloc, "test-key", 8, prep.output, prep.output_len, &tts_cfg, "mp3",
        &audio_bytes, &audio_len);
    HU_ASSERT_EQ(tts_err, HU_OK);
    HU_ASSERT_NOT_NULL(audio_bytes);
    HU_ASSERT_EQ(audio_len, 400u);
    hu_cartesia_tts_free_bytes(&alloc, audio_bytes, audio_len);
#endif

    hu_agent_clear_pending_voice();
    hu_agent_clear_current_for_tools();
}

/* ── Test: strip-SSML fallback mode in preprocessor pipeline ──────────── */

static void test_pipeline_with_preprocessor_strip_ssml(void) {
    setup();

    hu_tool_t tool = {0};
    hu_send_voice_message_create(&alloc, &tool);
    hu_agent_t fake_agent = {0};
    hu_agent_set_current_for_tools(&fake_agent);
    hu_agent_clear_pending_voice();

    const char *json =
        "{\"transcript\":\"I'm so sorry about what happened. "
        "But honestly, you handled it with so much grace. "
        "I'm really proud of you!\"}";
    hu_json_value_t *parsed = NULL;
    hu_json_parse(&alloc, json, strlen(json), &parsed);
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, parsed, &result);
    HU_ASSERT_TRUE(result.success);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, parsed);

    size_t pv_len = 0;
    const char *pv_transcript = hu_agent_pending_voice_transcript(&pv_len);
    HU_ASSERT_NOT_NULL(pv_transcript);

    hu_prep_config_t prep_cfg = {
        .incoming_msg = "I lost my job today",
        .incoming_msg_len = 19,
        .base_speed = 0.95f,
        .pause_factor = 1.2f,
        .discourse_rate = 0.0f,
        .nonverbals_enabled = true,
        .strip_ssml = true,
        .seed = 42,
        .hour_local = 14,
    };
    hu_prep_result_t prep = {0};
    hu_error_t err = hu_transcript_prep(pv_transcript, pv_len, &prep_cfg, &prep);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(prep.output_len > 0);
    HU_ASSERT_TRUE(prep.sentence_count >= 2);
    HU_ASSERT_NOT_NULL(prep.dominant_emotion);

    HU_ASSERT_TRUE(strstr(prep.output, "<break") == NULL);
    HU_ASSERT_TRUE(strstr(prep.output, "<emotion") == NULL);
    HU_ASSERT_TRUE(strstr(prep.output, "<speed") == NULL);
    HU_ASSERT_TRUE(strstr(prep.output, "<volume") == NULL);

    HU_ASSERT_TRUE(strstr(prep.output, "sorry") != NULL || strstr(prep.output, "proud") != NULL);

#if HU_ENABLE_CARTESIA
    hu_cartesia_tts_config_t tts_cfg = {
        .model_id = "sonic-3-2026-01-12",
        .voice_id = "test-voice-uuid",
        .emotion = prep.dominant_emotion,
        .speed = 0.95f,
        .volume = prep.volume,
        .nonverbals = true,
    };
    unsigned char *audio_bytes = NULL;
    size_t audio_len = 0;
    hu_error_t tts_err = hu_cartesia_tts_synthesize(
        &alloc, "test-key", 8, prep.output, prep.output_len, &tts_cfg, "mp3",
        &audio_bytes, &audio_len);
    HU_ASSERT_EQ(tts_err, HU_OK);
    HU_ASSERT_NOT_NULL(audio_bytes);
    HU_ASSERT_EQ(audio_len, 400u);
    hu_cartesia_tts_free_bytes(&alloc, audio_bytes, audio_len);
#endif

    hu_agent_clear_pending_voice();
    hu_agent_clear_current_for_tools();
}

/* ── Registration ────────────────────────────────────────────────────── */

void run_voice_message_integration_tests(void) {
    HU_TEST_SUITE("Voice message integration");

    HU_RUN_TEST(test_full_pipeline_tool_to_audio_file);
    HU_RUN_TEST(test_pipeline_auto_emotion_from_context);
    HU_RUN_TEST(test_pipeline_response_as_tts_source);
    HU_RUN_TEST(test_pipeline_imessage_selects_caf);
    HU_RUN_TEST(test_pipeline_telegram_selects_ogg);
    HU_RUN_TEST(test_pipeline_slack_selects_mp3);
    HU_RUN_TEST(test_pipeline_rate_limit_single_voice_per_turn);
    HU_RUN_TEST(test_pipeline_with_preprocessor);
    HU_RUN_TEST(test_pipeline_with_preprocessor_strip_ssml);
}
