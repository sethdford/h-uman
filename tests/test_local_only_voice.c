/* tests/test_local_only_voice.c
 *
 * Owner ruling 2026-10-02, "allow voice services only": under local_only the
 * only content that may leave is reply text to the allow-listed TTS service
 * and inbound audio to the allow-listed STT service. Everything else that
 * carries content is refused, including paths outside src/core/http.c:
 *   - websockets (Gemini Live, OpenAI Realtime, OpenAI ws_streaming chat),
 *   - the spawned-curl voice paths (OpenAI /audio/speech TTS, generic STT,
 *     Gemini STT),
 *   - the web_search tool (its query is written from the conversation).
 * Each refusal is pinned against the real production entry point with an OFF
 * (or allow-listed) arm that succeeds, so a pass is never vacuous.
 * Hermetic: HU_IS_TEST mocks every transport; no network, no processes.
 */

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/core/local_only_guard.h"
#include "human/tools/web_search.h"
#include "human/tts/cartesia.h"
#include "human/voice.h"
#include "human/websocket/websocket.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#define GEMINI_LIVE_URL                           \
    "wss://generativelanguage.googleapis.com/ws/" \
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
#define OPENAI_REALTIME_URL "wss://api.openai.com/v1/realtime"
#define OPENAI_WS_CHAT_URL  "wss://api.openai.com/v1/chat/completions"
#define CARTESIA_WS_URL     "wss://api.cartesia.ai/tts/websocket"

static const char *const k_default_allow[] = {"tts:cartesia", "stt:cartesia"};

static void lo_clean(void) {
    unsetenv("HU_LOCAL_ONLY");
    hu_local_only_reset();
    (void)hu_local_only_set_caller(NULL);
}

static void lo_live_default(void) {
    lo_clean();
    hu_local_only_configure(HU_GATE_LIVE);
    hu_local_only_set_allow(k_default_allow, 2);
}

static void voice_service_names_come_from_the_endpoint(void) {
    char svc[64];
    HU_ASSERT_TRUE(
        hu_local_only_voice_service("https://api.cartesia.ai/tts/bytes", svc, sizeof(svc)));
    HU_ASSERT_STR_EQ(svc, "tts:cartesia");
    HU_ASSERT_TRUE(hu_local_only_voice_service(CARTESIA_WS_URL, svc, sizeof(svc)));
    HU_ASSERT_STR_EQ(svc, "tts:cartesia");
    HU_ASSERT_TRUE(hu_local_only_voice_service("https://api.cartesia.ai/stt", svc, sizeof(svc)));
    HU_ASSERT_STR_EQ(svc, "stt:cartesia");
    HU_ASSERT_TRUE(hu_local_only_voice_service(
        "https://api.groq.com/openai/v1/audio/transcriptions", svc, sizeof(svc)));
    HU_ASSERT_STR_EQ(svc, "stt:groq");
    HU_ASSERT_TRUE(
        hu_local_only_voice_service("https://api.openai.com/v1/audio/speech", svc, sizeof(svc)));
    HU_ASSERT_STR_EQ(svc, "tts:openai");
    HU_ASSERT_TRUE(hu_local_only_voice_service("https://speech.googleapis.com/v1/speech:recognize",
                                               svc, sizeof(svc)));
    HU_ASSERT_STR_EQ(svc, "stt:googleapis");
    /* Conversational voice and chat are not voice services. */
    HU_ASSERT_FALSE(hu_local_only_voice_service(GEMINI_LIVE_URL, svc, sizeof(svc)));
    HU_ASSERT_FALSE(hu_local_only_voice_service(OPENAI_REALTIME_URL, svc, sizeof(svc)));
    HU_ASSERT_FALSE(hu_local_only_voice_service(OPENAI_WS_CHAT_URL, svc, sizeof(svc)));
    HU_ASSERT_FALSE(hu_local_only_voice_service("https://hnrss.org/frontpage", svc, sizeof(svc)));
}

static void allow_list_membership(void) {
    lo_clean();
    HU_ASSERT_FALSE(hu_local_only_service_allowed("tts:cartesia"));
    hu_local_only_set_allow(k_default_allow, 2);
    HU_ASSERT_TRUE(hu_local_only_service_allowed("tts:cartesia"));
    HU_ASSERT_TRUE(hu_local_only_service_allowed("stt:cartesia"));
    HU_ASSERT_FALSE(hu_local_only_service_allowed("tts:openai"));
    HU_ASSERT_FALSE(hu_local_only_service_allowed("tts:cartesia-evil"));
    lo_clean();
    HU_ASSERT_FALSE(hu_local_only_service_allowed("tts:cartesia")); /* reset clears it */
}

/* (a) Cartesia TTS: reply text -> audio is the one allowed TTS. */
static void cartesia_tts_allowed_only_when_listed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char say[] = "on my way";
    hu_cartesia_tts_config_t tc = {0};
    unsigned char *bytes = NULL;
    size_t blen = 0;

    lo_live_default();
    HU_ASSERT_EQ((int)hu_cartesia_tts_synthesize(&alloc, "test-key", 8, say, sizeof(say) - 1, &tc,
                                                 "mp3", &bytes, &blen),
                 (int)HU_OK);
    HU_ASSERT_NOT_NULL(bytes);
    hu_cartesia_tts_free_bytes(&alloc, bytes, blen);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u);

    hu_local_only_set_allow(NULL, 0); /* same call, nothing listed: refused */
    bytes = NULL;
    blen = 0;
    HU_ASSERT_EQ((int)hu_cartesia_tts_synthesize(&alloc, "test-key", 8, say, sizeof(say) - 1, &tc,
                                                 "mp3", &bytes, &blen),
                 (int)HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_NULL(bytes);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 1u);
    lo_clean();
}

/* The OpenAI /audio/speech fallback TTS is NOT allowed: Cartesia is the one. */
static void openai_speech_tts_refused(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_config_t vc = {0};
    vc.api_key = "test-key";
    vc.api_key_len = 8;
    void *audio = NULL;
    size_t alen = 0;

    lo_clean(); /* OFF: the fallback runs */
    HU_ASSERT_EQ((int)hu_voice_tts(&alloc, &vc, "hey", 3, &audio, &alen), (int)HU_OK);
    HU_ASSERT_NOT_NULL(audio);
    alloc.free(alloc.ctx, audio, alen);

    lo_live_default();
    audio = NULL;
    alen = 0;
    HU_ASSERT_EQ((int)hu_voice_tts(&alloc, &vc, "hey", 3, &audio, &alen),
                 (int)HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_NULL(audio);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 1u);
    lo_clean();
}

static void stt_once(hu_voice_config_t *vc, hu_error_t want) {
    hu_allocator_t alloc = hu_system_allocator();
    char *text = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ((int)hu_voice_stt_file(&alloc, vc, "/tmp/inbound.m4a", &text, &tlen), (int)want);
    if (text)
        alloc.free(alloc.ctx, text, tlen + 1);
}

/* (b) Inbound STT is allowed only for the listed provider. */
static void inbound_stt_only_for_listed_provider(void) {
    hu_voice_config_t cart = {0};
    cart.stt_provider = "cartesia";
    cart.cartesia_api_key = "test-key";
    cart.cartesia_api_key_len = 8;
    hu_voice_config_t groq = {0}; /* the generic path's default endpoint is Groq */
    groq.api_key = "test-key";
    groq.api_key_len = 8;

    lo_live_default();
    stt_once(&cart, HU_OK);
    stt_once(&groq, HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 1u);

    static const char *const groq_only[] = {"stt:groq"};
    hu_local_only_set_allow(groq_only, 1);
    stt_once(&groq, HU_OK);
    stt_once(&cart, HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 2u);
    lo_clean();
}

/* Gemini audio/video understanding is a generateContent model request. */
static void gemini_stt_refused(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_config_t vc = {0};
    vc.api_key = "test-key";
    vc.api_key_len = 8;
    char *text = NULL;
    size_t tlen = 0;
    lo_clean();
    HU_ASSERT_EQ((int)hu_voice_stt_gemini(&alloc, &vc, "AAAA", 4, "audio/m4a", &text, &tlen),
                 (int)HU_OK);
    alloc.free(alloc.ctx, text, tlen + 1);
    lo_live_default();
    text = NULL;
    HU_ASSERT_EQ((int)hu_voice_stt_gemini(&alloc, &vc, "AAAA", 4, "audio/m4a", &text, &tlen),
                 (int)HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_NULL(text);
    lo_clean();
}

static hu_error_t ws_try(const char *url) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_client_t *ws = NULL;
    hu_error_t err = hu_ws_connect(&alloc, url, &ws);
    if (ws)
        hu_ws_close(ws, &alloc);
    return err;
}

static void gemini_live_ws_refused(void) {
    lo_clean();
    HU_ASSERT_EQ((int)ws_try(GEMINI_LIVE_URL), (int)HU_OK); /* OFF */
    lo_live_default();
    HU_ASSERT_EQ((int)ws_try(GEMINI_LIVE_URL), (int)HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_EQ((int)ws_try(OPENAI_REALTIME_URL), (int)HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 2u);
    lo_clean();
}

static void openai_ws_streaming_refused(void) {
    lo_clean();
    HU_ASSERT_EQ((int)ws_try(OPENAI_WS_CHAT_URL), (int)HU_OK); /* OFF */
    lo_live_default();
    HU_ASSERT_EQ((int)ws_try(OPENAI_WS_CHAT_URL), (int)HU_ERR_PERMISSION_DENIED);
    /* ws_streaming to the local server stays allowed. */
    HU_ASSERT_EQ((int)ws_try("ws://127.0.0.1:8741/v1/chat/completions"), (int)HU_OK);
    /* And the Cartesia streaming TTS socket is the allowed TTS. */
    HU_ASSERT_EQ((int)ws_try(CARTESIA_WS_URL), (int)HU_OK);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 1u);
    lo_clean();
}

/* Audit mode must see the ws path too, and refuse nothing. */
static void audit_sees_ws_and_curl_paths(void) {
    lo_clean();
    hu_local_only_configure(HU_GATE_SHADOW);
    hu_local_only_set_allow(k_default_allow, 2);
    const char *prev = hu_local_only_set_caller("voice");
    HU_ASSERT_EQ((int)ws_try(GEMINI_LIVE_URL), (int)HU_OK);
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_config_t vc = {0};
    vc.api_key = "test-key";
    vc.api_key_len = 8;
    void *audio = NULL;
    size_t alen = 0;
    HU_ASSERT_EQ((int)hu_voice_tts(&alloc, &vc, "hey", 3, &audio, &alen), (int)HU_OK);
    alloc.free(alloc.ctx, audio, alen);
    (void)hu_local_only_set_caller(prev);
    HU_ASSERT_EQ(hu_local_only_audit_count(), 2u);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u);
    lo_clean();
}

/* Ruling: a web_search query is written by the model from the conversation,
 * so it is content. Refused unless "tool:web_search" is allow-listed. */
static void web_search_tool_refused(void) {
    hu_allocator_t alloc = hu_system_allocator();
    lo_live_default();
    hu_tool_t tool;
    HU_ASSERT_EQ(hu_web_search_create(&alloc, NULL, NULL, 0, &tool), HU_OK);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "query", hu_json_string_new(&alloc, "dinner near me", 14));
    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ((int)tool.vtable->execute(tool.ctx, &alloc, args, &result), (int)HU_OK);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT_NOT_NULL(strstr(result.error_msg, "local_only"));
    HU_ASSERT_EQ(hu_local_only_refused_count(), 1u);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
    lo_clean();
}

void run_local_only_voice_tests(void) {
    HU_TEST_SUITE("local_only_voice");
    HU_RUN_TEST(voice_service_names_come_from_the_endpoint);
    HU_RUN_TEST(allow_list_membership);
    HU_RUN_TEST(cartesia_tts_allowed_only_when_listed);
    HU_RUN_TEST(openai_speech_tts_refused);
    HU_RUN_TEST(inbound_stt_only_for_listed_provider);
    HU_RUN_TEST(gemini_stt_refused);
    HU_RUN_TEST(gemini_live_ws_refused);
    HU_RUN_TEST(openai_ws_streaming_refused);
    HU_RUN_TEST(audit_sees_ws_and_curl_paths);
    HU_RUN_TEST(web_search_tool_refused);
}
