/* tests/test_daemon_outbound_bus.c — outbound bus bridge (E2 carve-out).
 *
 * Pins src/daemon/daemon_outbound_bus.c: the UTF-8-safe clamp into
 * hu_bus_event_t.message, channel lookup by vtable name, the stream-event →
 * bus mapping, and the bus → channel delivery rules the service loop relies
 * on: a chunk starts typing once and goes out via send_event at CHUNK stage;
 * a final message goes via send, stops typing and flags
 * text_delivered_via_bus; iMessage finals are deferred to the post-turn
 * dispatcher (never sent here, flag stays false). */
#include "human/daemon_outbound_bus.h"
#include "test_framework.h"

#include <string.h>

/* ── Mock channel ─────────────────────────────────────────────────────── */

typedef struct mock_chan {
    const char *name;
    int send_calls;
    int send_event_calls;
    int start_typing_calls;
    int stop_typing_calls;
    hu_outbound_stage_t last_stage;
    char last_msg[HU_BUS_MSG_LEN];
    char last_target[HU_BUS_ID_LEN];
} mock_chan_t;

static const char *mock_name(void *ctx) {
    return ((mock_chan_t *)ctx)->name;
}

static void mock_capture(mock_chan_t *m, const char *target, size_t target_len, const char *msg,
                         size_t msg_len) {
    size_t t = target_len < sizeof(m->last_target) - 1 ? target_len : sizeof(m->last_target) - 1;
    memcpy(m->last_target, target, t);
    m->last_target[t] = '\0';
    size_t n = msg_len < sizeof(m->last_msg) - 1 ? msg_len : sizeof(m->last_msg) - 1;
    memcpy(m->last_msg, msg, n);
    m->last_msg[n] = '\0';
}

static hu_error_t mock_send(void *ctx, const char *target, size_t target_len, const char *msg,
                            size_t msg_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    mock_chan_t *m = (mock_chan_t *)ctx;
    m->send_calls++;
    mock_capture(m, target, target_len, msg, msg_len);
    return HU_OK;
}

static hu_error_t mock_send_event(void *ctx, const char *target, size_t target_len, const char *msg,
                                  size_t msg_len, const char *const *media, size_t media_count,
                                  hu_outbound_stage_t stage) {
    (void)media;
    (void)media_count;
    mock_chan_t *m = (mock_chan_t *)ctx;
    m->send_event_calls++;
    m->last_stage = stage;
    mock_capture(m, target, target_len, msg, msg_len);
    return HU_OK;
}

static hu_error_t mock_start_typing(void *ctx, const char *r, size_t rl) {
    (void)r;
    (void)rl;
    ((mock_chan_t *)ctx)->start_typing_calls++;
    return HU_OK;
}

static hu_error_t mock_stop_typing(void *ctx, const char *r, size_t rl) {
    (void)r;
    (void)rl;
    ((mock_chan_t *)ctx)->stop_typing_calls++;
    return HU_OK;
}

/* send-only channel: no send_event, no typing (the "plain" vtable shape). */
static const hu_channel_vtable_t mock_vt_plain = {
    .send = mock_send,
    .name = mock_name,
};

/* streaming channel: send_event + typing indicators. */
static const hu_channel_vtable_t mock_vt_stream = {
    .send = mock_send,
    .name = mock_name,
    .send_event = mock_send_event,
    .start_typing = mock_start_typing,
    .stop_typing = mock_stop_typing,
};

/* ── Bus capture subscriber ───────────────────────────────────────────── */

static int cap_count;
static hu_bus_event_t cap_last;

static bool capture_fn(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx) {
    (void)user_ctx;
    (void)type;
    cap_count++;
    cap_last = *ev;
    return true;
}

static void capture_reset(hu_bus_t *bus) {
    hu_bus_init(bus);
    cap_count = 0;
    memset(&cap_last, 0, sizeof(cap_last));
    HU_ASSERT_EQ(hu_bus_subscribe(bus, capture_fn, NULL, HU_BUS_EVENT_COUNT), HU_OK);
}

/* ── utf8_safe_truncate + bus_set_message ─────────────────────────────── */

static void test_utf8_truncate_backs_off_to_character_boundary(void) {
    /* "aé" = 61 C3 A9. Cutting after C3 must back off to 1. */
    const char s[] = "a\xC3\xA9";
    HU_ASSERT_EQ(hu_daemon_outbound_utf8_safe_truncate(s, 0), 0u);
    HU_ASSERT_EQ(hu_daemon_outbound_utf8_safe_truncate(s, 1), 1u);
    HU_ASSERT_EQ(hu_daemon_outbound_utf8_safe_truncate(s, 2), 1u);
    /* 4-byte emoji F0 9F 98 80 truncated at 2 or 3 bytes → 0 (lead dropped). */
    const char e[] = "\xF0\x9F\x98\x80";
    HU_ASSERT_EQ(hu_daemon_outbound_utf8_safe_truncate(e, 2), 0u);
    HU_ASSERT_EQ(hu_daemon_outbound_utf8_safe_truncate(e, 3), 0u);
}

static void test_bus_set_message_clamps_on_utf8_boundary(void) {
    hu_bus_event_t ev;
    memset(&ev, 'x', sizeof(ev));
    hu_daemon_outbound_bus_set_message(&ev, NULL, 5);
    HU_ASSERT_EQ(ev.message[0], '\0');
    hu_daemon_outbound_bus_set_message(&ev, "hi", 2);
    HU_ASSERT_STR_EQ(ev.message, "hi");

    /* HU_BUS_MSG_LEN-2 ASCII bytes followed by a 2-byte char: the naive clamp
     * at HU_BUS_MSG_LEN-1 would land between C3 and A9; the whole char must go. */
    static char big[HU_BUS_MSG_LEN + 8];
    memset(big, 'a', HU_BUS_MSG_LEN - 2);
    big[HU_BUS_MSG_LEN - 2] = '\xC3';
    big[HU_BUS_MSG_LEN - 1] = '\xA9';
    big[HU_BUS_MSG_LEN] = 'z';
    hu_daemon_outbound_bus_set_message(&ev, big, HU_BUS_MSG_LEN + 1);
    HU_ASSERT_EQ(strlen(ev.message), (size_t)HU_BUS_MSG_LEN - 2);
    HU_ASSERT_EQ(ev.message[HU_BUS_MSG_LEN - 3], 'a');
}

/* ── find_channel ─────────────────────────────────────────────────────── */

static void test_find_channel_matches_vtable_name(void) {
    mock_chan_t a = {.name = "discord"}, b = {.name = "slack"};
    hu_channel_t ca = {.ctx = &a, .vtable = &mock_vt_plain};
    hu_channel_t cb = {.ctx = &b, .vtable = &mock_vt_plain};
    hu_service_channel_t chans[3];
    memset(chans, 0, sizeof(chans));
    chans[0].channel = NULL; /* skipped, not dereferenced */
    chans[1].channel = &ca;
    chans[2].channel = &cb;
    HU_ASSERT_TRUE(hu_daemon_outbound_find_channel(chans, 3, "slack") == &chans[2]);
    HU_ASSERT_TRUE(hu_daemon_outbound_find_channel(chans, 3, "discord") == &chans[1]);
    HU_ASSERT_NULL(hu_daemon_outbound_find_channel(chans, 3, "telegram"));
    HU_ASSERT_NULL(hu_daemon_outbound_find_channel(chans, 3, ""));
    HU_ASSERT_NULL(hu_daemon_outbound_find_channel(chans, 0, "slack"));
    HU_ASSERT_NULL(hu_daemon_outbound_find_channel(NULL, 3, "slack"));
}

/* ── stream_event_cb ──────────────────────────────────────────────────── */

static void test_stream_event_maps_thinking_and_tools_to_bus(void) {
    hu_bus_t bus;
    capture_reset(&bus);
    hu_daemon_stream_ctx_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.bus = &bus;
    memcpy(sc.channel, "cli", 3);
    memcpy(sc.id, "sess-1", 6);

    hu_agent_stream_event_t ev = {.type = HU_AGENT_STREAM_THINKING, .data = "hmm", .data_len = 3};
    hu_daemon_outbound_stream_event_cb(&ev, &sc);
    HU_ASSERT_EQ(cap_count, 1);
    HU_ASSERT_EQ((int)cap_last.type, (int)HU_BUS_THINKING_CHUNK);
    HU_ASSERT_STR_EQ(cap_last.message, "hmm");
    HU_ASSERT_STR_EQ(cap_last.channel, "cli");
    HU_ASSERT_STR_EQ(cap_last.id, "sess-1");

    hu_agent_stream_event_t ts = {
        .type = HU_AGENT_STREAM_TOOL_START, .tool_name = "shell", .tool_name_len = 5};
    hu_daemon_outbound_stream_event_cb(&ts, &sc);
    HU_ASSERT_EQ(cap_count, 2);
    HU_ASSERT_EQ((int)cap_last.type, (int)HU_BUS_TOOL_CALL);
    HU_ASSERT_STR_EQ(cap_last.message, "shell");

    hu_agent_stream_event_t tr = {.type = HU_AGENT_STREAM_TOOL_RESULT, .data = "ok", .data_len = 2};
    hu_daemon_outbound_stream_event_cb(&tr, &sc);
    HU_ASSERT_EQ(cap_count, 3);
    HU_ASSERT_EQ((int)cap_last.type, (int)HU_BUS_TOOL_CALL_RESULT);

    /* Empty text and thinking publish nothing; a NULL ctx is ignored. */
    hu_agent_stream_event_t empty = {.type = HU_AGENT_STREAM_TEXT, .data = "", .data_len = 0};
    hu_daemon_outbound_stream_event_cb(&empty, &sc);
    hu_agent_stream_event_t et = {.type = HU_AGENT_STREAM_THINKING, .data = NULL, .data_len = 0};
    hu_daemon_outbound_stream_event_cb(&et, &sc);
    hu_daemon_outbound_stream_event_cb(&ev, NULL);
    HU_ASSERT_EQ(cap_count, 3);
}

static void test_stream_event_text_without_alloc_strips_tags_and_publishes(void) {
    hu_bus_t bus;
    capture_reset(&bus);
    hu_daemon_stream_ctx_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.bus = &bus; /* alloc NULL → defensive strip arm, no validator chain */

    hu_agent_stream_event_t ev = {
        .type = HU_AGENT_STREAM_TEXT, .data = "plain words", .data_len = 11};
    hu_daemon_outbound_stream_event_cb(&ev, &sc);
    HU_ASSERT_EQ(cap_count, 1);
    HU_ASSERT_EQ((int)cap_last.type, (int)HU_BUS_MESSAGE_CHUNK);
    HU_ASSERT_STR_EQ(cap_last.message, "plain words");

    /* A chunk that is entirely a stripped artifact tag never reaches the bus. */
    const char *tagged = "<thinking>secret</thinking>";
    hu_agent_stream_event_t tv = {
        .type = HU_AGENT_STREAM_TEXT, .data = tagged, .data_len = strlen(tagged)};
    hu_daemon_outbound_stream_event_cb(&tv, &sc);
    HU_ASSERT_EQ(cap_count, 1);
}

/* ── outbound_bus_cb ──────────────────────────────────────────────────── */

typedef struct fixture {
    mock_chan_t m;
    hu_channel_t ch;
    hu_service_channel_t sch[1];
    hu_daemon_out_turn_state_t turn;
    hu_daemon_out_bus_bridge_t br;
} fixture_t;

static void fixture_init(fixture_t *f, const char *name, const hu_channel_vtable_t *vt) {
    memset(f, 0, sizeof(*f));
    f->m.name = name;
    f->ch.ctx = &f->m;
    f->ch.vtable = vt;
    f->sch[0].channel = &f->ch;
    f->br.channels = f->sch;
    f->br.channel_count = 1;
    f->br.active_turn = &f->turn;
    f->br.delivery_turn = &f->turn;
}

static hu_bus_event_t make_event(hu_bus_event_type_t type, const char *channel, const char *id,
                                 const char *message) {
    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    strncpy(ev.channel, channel, HU_BUS_CHANNEL_LEN - 1);
    strncpy(ev.id, id, HU_BUS_ID_LEN - 1);
    strncpy(ev.message, message, HU_BUS_MSG_LEN - 1);
    return ev;
}

static void test_outbound_final_uses_send_and_flags_delivery(void) {
    fixture_t f;
    fixture_init(&f, "mock", &mock_vt_plain);
    hu_bus_event_t ev = make_event(HU_BUS_MESSAGE_SENT, "mock", "+15551234567", "final reply");

    HU_ASSERT_FALSE(f.turn.text_delivered_via_bus);
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, &ev, &f.br));
    HU_ASSERT_EQ(f.m.send_calls, 1);
    HU_ASSERT_EQ(f.m.send_event_calls, 0);
    HU_ASSERT_STR_EQ(f.m.last_msg, "final reply");
    HU_ASSERT_STR_EQ(f.m.last_target, "+15551234567");
    HU_ASSERT_TRUE(f.turn.text_delivered_via_bus);

    /* payload, when set, wins over message. */
    f.m.send_calls = 0;
    ev.payload = (void *)"from payload";
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, &ev, &f.br));
    HU_ASSERT_EQ(f.m.send_calls, 1);
    HU_ASSERT_STR_EQ(f.m.last_msg, "from payload");
}

static void test_outbound_chunk_starts_typing_once_and_streams(void) {
    fixture_t f;
    fixture_init(&f, "mock", &mock_vt_stream);
    hu_bus_event_t c1 = make_event(HU_BUS_MESSAGE_CHUNK, "mock", "u1", "hel");
    hu_bus_event_t c2 = make_event(HU_BUS_MESSAGE_CHUNK, "mock", "u1", "lo");

    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_CHUNK, &c1, &f.br));
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_CHUNK, &c2, &f.br));
    HU_ASSERT_EQ(f.m.start_typing_calls, 1);
    HU_ASSERT_TRUE(f.turn.typing_started);
    HU_ASSERT_EQ(f.m.send_event_calls, 2);
    HU_ASSERT_EQ((int)f.m.last_stage, (int)HU_OUTBOUND_STAGE_CHUNK);
    HU_ASSERT_STR_EQ(f.m.last_msg, "lo");
    HU_ASSERT_EQ(f.m.send_calls, 0);
    HU_ASSERT_FALSE(f.turn.text_delivered_via_bus); /* chunks never flag delivery */

    /* Final on a streaming channel: send_event at FINAL stage, typing stopped. */
    hu_bus_event_t fin = make_event(HU_BUS_MESSAGE_SENT, "mock", "u1", "hello");
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, &fin, &f.br));
    HU_ASSERT_EQ(f.m.send_event_calls, 3);
    HU_ASSERT_EQ((int)f.m.last_stage, (int)HU_OUTBOUND_STAGE_FINAL);
    HU_ASSERT_EQ(f.m.stop_typing_calls, 1);
    HU_ASSERT_TRUE(f.turn.text_delivered_via_bus);

    /* A chunk for a channel with no send_event is dropped silently. */
    fixture_t p;
    fixture_init(&p, "mock", &mock_vt_plain);
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_CHUNK, &c1, &p.br));
    HU_ASSERT_EQ(p.m.send_calls, 0);
    HU_ASSERT_FALSE(p.turn.typing_started);
}

static void test_outbound_imessage_final_is_deferred_to_dispatcher(void) {
    fixture_t f;
    fixture_init(&f, "imessage", &mock_vt_plain);
    hu_bus_event_t ev = make_event(HU_BUS_MESSAGE_SENT, "imessage", "+1555", "threaded?");

    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, &ev, &f.br));
    HU_ASSERT_EQ(f.m.send_calls, 0);
    HU_ASSERT_FALSE(f.turn.text_delivered_via_bus);
}

static void test_outbound_ignores_other_events_unknown_channels_and_empty(void) {
    fixture_t f;
    fixture_init(&f, "mock", &mock_vt_plain);
    hu_bus_event_t other = make_event(HU_BUS_MESSAGE_RECEIVED, "mock", "u", "in");
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_RECEIVED, &other, &f.br));
    hu_bus_event_t unknown = make_event(HU_BUS_MESSAGE_SENT, "nope", "u", "x");
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, &unknown, &f.br));
    hu_bus_event_t empty = make_event(HU_BUS_MESSAGE_SENT, "mock", "u", "");
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, &empty, &f.br));
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, NULL, &f.br));
    HU_ASSERT_TRUE(hu_daemon_outbound_bus_cb(HU_BUS_MESSAGE_SENT, &other, NULL));
    HU_ASSERT_EQ(f.m.send_calls, 0);
    HU_ASSERT_FALSE(f.turn.text_delivered_via_bus);
}

void run_daemon_outbound_bus_tests(void) {
    HU_RUN_TEST(test_utf8_truncate_backs_off_to_character_boundary);
    HU_RUN_TEST(test_bus_set_message_clamps_on_utf8_boundary);
    HU_RUN_TEST(test_find_channel_matches_vtable_name);
    HU_RUN_TEST(test_stream_event_maps_thinking_and_tools_to_bus);
    HU_RUN_TEST(test_stream_event_text_without_alloc_strips_tags_and_publishes);
    HU_RUN_TEST(test_outbound_final_uses_send_and_flags_delivery);
    HU_RUN_TEST(test_outbound_chunk_starts_typing_once_and_streams);
    HU_RUN_TEST(test_outbound_imessage_final_is_deferred_to_dispatcher);
    HU_RUN_TEST(test_outbound_ignores_other_events_unknown_channels_and_empty);
}
