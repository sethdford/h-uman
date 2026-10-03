/* Gateway voice control-protocol tests (mock STT, no network). */
#include "human/bus.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/json.h"
#include "human/gateway/control_protocol.h"
#include "human/gateway/voice_stream.h"
#include "test_framework.h"
#include <string.h>

#ifdef HU_GATEWAY_POSIX
#include "cp_internal.h"
#endif

#ifdef HU_GATEWAY_POSIX
static void assert_transcribe_json_text(hu_allocator_t *alloc, const char *json, size_t json_len,
                                        const char *expect_text) {
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(alloc, json, json_len, &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    hu_json_value_t *textv = hu_json_object_get(root, "text");
    HU_ASSERT_NOT_NULL(textv);
    HU_ASSERT_EQ(textv->type, HU_JSON_STRING);
    HU_ASSERT_STR_EQ(textv->data.string.ptr, expect_text);
    hu_json_free(alloc, root);
}

/* Replies are written like texts; the voice must not read "lol" or emoji aloud. */
static void voice_stream_speakable_drops_texting_forms(void) {
    char out[256];
    const char *reply = "haha yeah that was wild lol \xF0\x9F\x98\x82 lmk when you're free";
    size_t n = hu_voice_stream_speakable(reply, strlen(reply), out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_EQ(n, strlen(out));
    HU_ASSERT_NULL(strstr(out, "lol"));
    HU_ASSERT_NULL(strstr(out, "haha"));
    HU_ASSERT_NULL(strstr(out, "\xF0\x9F\x98\x82"));
    HU_ASSERT_STR_CONTAINS(out, "that was wild");
    HU_ASSERT_STR_CONTAINS(out, "let me know");
}

static void voice_stream_speakable_skips_a_chunk_with_nothing_to_say(void) {
    char out[64];
    HU_ASSERT_EQ(hu_voice_stream_speakable("lol", 3, out, sizeof(out)), 0u);
    HU_ASSERT_EQ(hu_voice_stream_speakable(NULL, 0, out, sizeof(out)), 0u);
    HU_ASSERT_EQ(out[0], '\0');
}

static void test_gateway_voice_transcribe_cartesia_provider(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"voice\":{\"stt_provider\":\"cartesia\"},"
                       "\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}]}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    const char *req = "{\"method\":\"voice.transcribe\",\"params\":{\"audio\":\"ZGVzdA==\","
                      "\"mimeType\":\"audio/webm\"}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, req, strlen(req), &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_voice_transcribe(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    assert_transcribe_json_text(&backing, out, out_len, "Cartesia mock transcription");
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_transcribe_default_gemini(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"gemini\",\"api_key\":\"gk-test\"}]}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    const char *req = "{\"method\":\"voice.transcribe\",\"params\":{\"audio\":\"ZGVzdA==\","
                      "\"mimeType\":\"audio/webm\"}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, req, strlen(req), &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_voice_transcribe(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    assert_transcribe_json_text(&backing, out, out_len, "Mock Gemini transcription");
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_session_start_returns_pcm_meta(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}],"
                       "\"voice\":{\"tts_voice\":\"v1\",\"tts_model\":\"m1\"}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    conn.id = 42;
    conn.active = true;
    const char *req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.start\","
                      "\"params\":{\"voiceId\":\"vid\",\"modelId\":\"mid\"}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, req, strlen(req), &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "pcm_f32le") != NULL);
    HU_ASSERT_TRUE(strstr(out, "24000") != NULL);
    HU_ASSERT_TRUE(strstr(out, "sessionId") != NULL);
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    const char *req2 =
        "{\"type\":\"req\",\"id\":\"2\",\"method\":\"voice.session.stop\",\"params\":{}}";
    HU_ASSERT_EQ(hu_json_parse(&backing, req2, strlen(req2), &root), HU_OK);
    out = NULL;
    out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_stop(&backing, &app, &conn, &proto, root, &out, &out_len), HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    hu_arena_destroy(arena);
}

static void test_gateway_voice_transcribe_missing_audio_returns_error(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"gemini\",\"api_key\":\"gk-test\"}]}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    const char *req = "{\"method\":\"voice.transcribe\",\"params\":{}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, req, strlen(req), &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_voice_transcribe(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}
static void test_gateway_voice_session_interrupt_clears_pcm(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}],"
                       "\"voice\":{\"tts_voice\":\"v1\",\"tts_model\":\"m1\"}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    conn.id = 50;
    conn.active = true;

    /* Start session */
    const char *start_req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.start\","
                            "\"params\":{}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, start_req, strlen(start_req), &root), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    /* Feed binary data */
    const char audio[] = "fake-audio-data";
    hu_voice_stream_on_binary(&proto, &conn, audio, sizeof(audio));

    /* Interrupt */
    const char *int_req = "{\"type\":\"req\",\"id\":\"2\",\"method\":\"voice.session.interrupt\","
                          "\"params\":{}}";
    HU_ASSERT_EQ(hu_json_parse(&backing, int_req, strlen(int_req), &root), HU_OK);
    out = NULL;
    out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_interrupt(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "true") != NULL);
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    /* Stop to clean up slot */
    const char *stop_req = "{\"type\":\"req\",\"id\":\"3\",\"method\":\"voice.session.stop\","
                           "\"params\":{}}";
    HU_ASSERT_EQ(hu_json_parse(&backing, stop_req, strlen(stop_req), &root), HU_OK);
    out = NULL;
    out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_stop(&backing, &app, &conn, &proto, root, &out, &out_len), HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_on_binary_accumulates(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}],"
                       "\"voice\":{\"tts_voice\":\"v1\",\"tts_model\":\"m1\"}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    proto.alloc = &backing;
    hu_ws_conn_t conn = {0};
    conn.id = 60;
    conn.active = true;

    /* Start session to allocate a slot */
    const char *start_req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.start\","
                            "\"params\":{}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, start_req, strlen(start_req), &root), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    /* Send two binary chunks */
    const char chunk1[] = "AAAA";
    const char chunk2[] = "BBBB";
    hu_voice_stream_on_binary(&proto, &conn, chunk1, 4);
    hu_voice_stream_on_binary(&proto, &conn, chunk2, 4);

    /* Without a conn slot, binary should be silently ignored */
    hu_ws_conn_t bogus = {0};
    bogus.id = 999;
    hu_voice_stream_on_binary(&proto, &bogus, chunk1, 4);

    /* Null/zero-length should be ignored without crash */
    hu_voice_stream_on_binary(&proto, &conn, NULL, 0);
    hu_voice_stream_on_binary(&proto, &conn, chunk1, 0);

    /* Stop to clean up */
    const char *stop_req = "{\"type\":\"req\",\"id\":\"2\",\"method\":\"voice.session.stop\","
                           "\"params\":{}}";
    HU_ASSERT_EQ(hu_json_parse(&backing, stop_req, strlen(stop_req), &root), HU_OK);
    out = NULL;
    out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_stop(&backing, &app, &conn, &proto, root, &out, &out_len), HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_on_conn_close_frees_slot(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}],"
                       "\"voice\":{\"tts_voice\":\"v1\",\"tts_model\":\"m1\"}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    proto.alloc = &backing;
    hu_ws_conn_t conn = {0};
    conn.id = 70;
    conn.active = true;

    /* Start session */
    const char *start_req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.start\","
                            "\"params\":{}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, start_req, strlen(start_req), &root), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    /* Simulate disconnect — should free the slot without crash */
    hu_voice_stream_on_conn_close(&conn);

    /* Calling again on same conn should be harmless (slot already freed) */
    hu_voice_stream_on_conn_close(&conn);

    /* A NULL conn should be harmless */
    hu_voice_stream_on_conn_close(NULL);

    hu_arena_destroy(arena);
}

/* voice.session.greet: Lester opens the conversation. Refused before a session exists;
 * after start it hands the greeting cue to the agent as a message on this session. */
typedef struct {
    int received;
    char id[64];
    char message[HU_BUS_MSG_LEN];
} greet_capture_t;

static bool greet_capture_cb(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *ctx) {
    greet_capture_t *c = (greet_capture_t *)ctx;
    if (type == HU_BUS_MESSAGE_RECEIVED && ev) {
        c->received++;
        snprintf(c->id, sizeof(c->id), "%s", ev->id);
        snprintf(c->message, sizeof(c->message), "%s", ev->message);
    }
    return true;
}

/* Cartesia closes an idle TTS socket: after ~6.7 min idle a reply had no audio. */
static void voice_stream_tts_is_reopened_only_after_a_long_idle(void) {
    int64_t t0 = 1000000;
    HU_ASSERT_FALSE(hu_voice_stream_tts_stale(t0, t0));
    HU_ASSERT_FALSE(hu_voice_stream_tts_stale(t0 + 30 * 1000, t0));
    HU_ASSERT_FALSE(hu_voice_stream_tts_stale(t0 + HU_VOICE_STREAM_TTS_IDLE_REOPEN_MS - 1, t0));
    HU_ASSERT_TRUE(hu_voice_stream_tts_stale(t0 + HU_VOICE_STREAM_TTS_IDLE_REOPEN_MS, t0));
    HU_ASSERT_TRUE(hu_voice_stream_tts_stale(t0 + 400 * 1000, t0));
}

static void gateway_voice_greet_starts_a_spoken_turn_after_session_start(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}],"
                       "\"voice\":{\"tts_voice\":\"v1\",\"tts_model\":\"m1\"}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);
    hu_bus_t bus;
    hu_bus_init(&bus);
    greet_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    HU_ASSERT_EQ(hu_bus_subscribe(&bus, greet_capture_cb, &cap, HU_BUS_EVENT_COUNT), HU_OK);
    hu_app_context_t app = {.config = &cfg, .alloc = &backing, .bus = &bus};
    hu_control_protocol_t proto = {0};
    proto.alloc = &backing;
    hu_ws_conn_t conn = {0};
    conn.id = 81;
    conn.active = true;
    char *out = NULL;
    size_t out_len = 0;

    /* No session yet: refused, nothing published. */
    HU_ASSERT_NEQ(cp_voice_session_greet(&backing, &app, &conn, &proto, NULL, &out, &out_len),
                  HU_OK);
    HU_ASSERT_EQ(cap.received, 0);

    hu_json_value_t *root = NULL;
    const char *start_req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.start\","
                            "\"params\":{}}";
    HU_ASSERT_EQ(hu_json_parse(&backing, start_req, strlen(start_req), &root), HU_OK);
    HU_ASSERT_EQ(cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    out = NULL;
    out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_greet(&backing, &app, &conn, &proto, NULL, &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "\"ok\":true");
    backing.free(backing.ctx, out, out_len + 1);
    HU_ASSERT_EQ(cap.received, 1);
    HU_ASSERT_STR_EQ(cap.id, "voice");
    HU_ASSERT_STR_CONTAINS(cap.message, "Say hello");

    hu_bus_unsubscribe(&bus, greet_capture_cb, &cap);
    hu_voice_stream_on_conn_close(&conn);
    hu_bus_deinit(&bus);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_audio_end_no_data_returns_error(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}],"
                       "\"voice\":{\"tts_voice\":\"v1\",\"tts_model\":\"m1\"}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_app_context_t app = {.config = &cfg, .alloc = &backing, .bus = &bus};
    hu_control_protocol_t proto = {0};
    proto.alloc = &backing;
    hu_ws_conn_t conn = {0};
    conn.id = 80;
    conn.active = true;

    /* Start session */
    const char *start_req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.start\","
                            "\"params\":{}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, start_req, strlen(start_req), &root), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    /* audio.end without any binary data should fail */
    const char *end_req = "{\"type\":\"req\",\"id\":\"2\",\"method\":\"voice.audio.end\","
                          "\"params\":{\"mimeType\":\"audio/webm\"}}";
    HU_ASSERT_EQ(hu_json_parse(&backing, end_req, strlen(end_req), &root), HU_OK);
    out = NULL;
    out_len = 0;
    hu_error_t err = cp_voice_audio_end(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    hu_json_free(&backing, root);

    /* Clean up */
    hu_voice_stream_on_conn_close(&conn);
    hu_bus_deinit(&bus);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_double_start_reuses_slot(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}],"
                       "\"voice\":{\"tts_voice\":\"v1\",\"tts_model\":\"m1\"}}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    conn.id = 90;
    conn.active = true;

    const char *start_req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.start\","
                            "\"params\":{}}";
    hu_json_value_t *root = NULL;
    char *out = NULL;
    size_t out_len = 0;

    /* First start */
    HU_ASSERT_EQ(hu_json_parse(&backing, start_req, strlen(start_req), &root), HU_OK);
    HU_ASSERT_EQ(cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    /* Second start on same conn — should succeed (reuse slot) */
    HU_ASSERT_EQ(hu_json_parse(&backing, start_req, strlen(start_req), &root), HU_OK);
    out = NULL;
    out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_start(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);

    /* Clean up */
    const char *stop_req = "{\"type\":\"req\",\"id\":\"2\",\"method\":\"voice.session.stop\","
                           "\"params\":{}}";
    HU_ASSERT_EQ(hu_json_parse(&backing, stop_req, strlen(stop_req), &root), HU_OK);
    out = NULL;
    out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_stop(&backing, &app, &conn, &proto, root, &out, &out_len), HU_OK);
    if (out)
        backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_interrupt_without_session_returns_ok(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}]}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    conn.id = 100;
    conn.active = true;

    /* Interrupt without a session — should return ok (no slot to interrupt) */
    const char *int_req = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"voice.session.interrupt\","
                          "\"params\":{}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, int_req, strlen(int_req), &root), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(cp_voice_session_interrupt(&backing, &app, &conn, &proto, root, &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "true") != NULL);
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}
static void test_gateway_voice_clone_returns_voice_id(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}]}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    const char *req = "{\"method\":\"voice.clone\",\"params\":{\"audio\":\"ZGVzdA==\","
                      "\"mimeType\":\"audio/wav\",\"name\":\"Test Voice\",\"language\":\"en\"}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, req, strlen(req), &root), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_voice_clone(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "voice_id") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Test Voice") != NULL);
    backing.free(backing.ctx, out, out_len);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_clone_missing_audio_returns_error(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}]}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    const char *req = "{\"method\":\"voice.clone\",\"params\":{\"name\":\"Test\"}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, req, strlen(req), &root), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_voice_clone(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_clone_missing_params_returns_error(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"cartesia\",\"api_key\":\"ck-test\"}]}";
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    hu_app_context_t app = {.config = &cfg, .alloc = &backing};
    hu_control_protocol_t proto = {0};
    hu_ws_conn_t conn = {0};
    const char *req = "{\"method\":\"voice.clone\"}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&backing, req, strlen(req), &root), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_voice_clone(&backing, &app, &conn, &proto, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&backing, root);
    hu_arena_destroy(arena);
}

static void test_gateway_voice_clone_null_alloc_returns_error(void) {
    hu_error_t err = cp_voice_clone(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

#endif /* HU_GATEWAY_POSIX */

void run_gateway_voice_tests(void) {
#ifdef HU_GATEWAY_POSIX
    HU_TEST_SUITE("Gateway voice");
    HU_RUN_TEST(voice_stream_speakable_drops_texting_forms);
    HU_RUN_TEST(voice_stream_speakable_skips_a_chunk_with_nothing_to_say);
    HU_RUN_TEST(test_gateway_voice_transcribe_cartesia_provider);
    HU_RUN_TEST(test_gateway_voice_transcribe_default_gemini);
    HU_RUN_TEST(test_gateway_voice_session_start_returns_pcm_meta);
    HU_RUN_TEST(test_gateway_voice_transcribe_missing_audio_returns_error);
    HU_RUN_TEST(test_gateway_voice_session_interrupt_clears_pcm);
    HU_RUN_TEST(test_gateway_voice_on_binary_accumulates);
    HU_RUN_TEST(test_gateway_voice_on_conn_close_frees_slot);
    HU_RUN_TEST(test_gateway_voice_audio_end_no_data_returns_error);
    HU_RUN_TEST(gateway_voice_greet_starts_a_spoken_turn_after_session_start);
    HU_RUN_TEST(voice_stream_tts_is_reopened_only_after_a_long_idle);
    HU_RUN_TEST(test_gateway_voice_double_start_reuses_slot);
    HU_RUN_TEST(test_gateway_voice_interrupt_without_session_returns_ok);
    HU_RUN_TEST(test_gateway_voice_clone_returns_voice_id);
    HU_RUN_TEST(test_gateway_voice_clone_missing_audio_returns_error);
    HU_RUN_TEST(test_gateway_voice_clone_missing_params_returns_error);
    HU_RUN_TEST(test_gateway_voice_clone_null_alloc_returns_error);
#endif
}
