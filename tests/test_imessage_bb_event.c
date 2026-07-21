/* Tests for the `imsg watch --bb-events` IMCore bridge event parser.
 *
 * Every FIXTURE_* string below is a VERBATIM stdout line captured from a live
 * `imsg watch --bb-events --reactions --json` on this Mac Studio (Tahoe
 * 26.5.1, SIP disabled, bridge connected) on 2026-07-19. Phone numbers are the
 * only edit. See docs/plans/2026-07-19-native-imessage/bb-events-schema.md.
 *
 * Same shape as test_imessage_caps.c: the parser is a pure function over
 * captured output, so the whole contract is testable with no bridge, no
 * subprocess and no chat.db. */

#include "human/channels/imessage_bb_event.h"
#include "test_framework.h"
#include <string.h>

/* ── Captured bridge-event lines ───────────────────────────────────────
 * NOTE the key ORDER differs between the first and the next two: Swift's
 * dictionary encoding does not preserve insertion order. These fixtures exist
 * precisely to pin that a reorder does not change the parse. */

static const char *FIXTURE_TYPING_START =
    "{\"event\":\"started-typing\",\"data\":{\"handle\":\"+15551234567\","
    "\"timestamp\":1784510000.5,\"chatGuid\":\"iMessage;-;+15551234567\"},"
    "\"kind\":\"bridge-event\"}";

static const char *FIXTURE_TYPING_STOP =
    "{\"kind\":\"bridge-event\",\"event\":\"stopped-typing\",\"data\":{"
    "\"timestamp\":1784510006.0999999,\"chatGuid\":\"iMessage;-;+15551234567\","
    "\"handle\":\"+15551234567\"}}";

static const char *FIXTURE_ALIASES_REMOVED =
    "{\"kind\":\"bridge-event\",\"event\":\"aliases-removed\",\"data\":{"
    "\"aliasType\":\"phone\",\"aliases\":[\"+15559876543\"]}}";

/* Envelope with no top-level `data` — observed when the inbox record carried
 * no payload; the CLI emits data:{} rather than dropping the line. */
static const char *FIXTURE_EMPTY_DATA =
    "{\"event\":\"started-typing\",\"data\":{},\"kind\":\"bridge-event\"}";

/* Unrecognised top-level event, still a bridge event by `kind`. */
static const char *FIXTURE_UNKNOWN_EVENT =
    "{\"event\":\"unknown\",\"data\":{},\"kind\":\"bridge-event\"}";

/* ── Captured NON-bridge lines from the SAME stream ────────────────────── */

static const char *FIXTURE_MESSAGE_ROW =
    "{\"guid\":\"229CFD8D-F2AD-48F1-81ED-50168D8B959B\",\"chat_id\":16,"
    "\"text\":\"bb-events probe B\",\"is_from_me\":true,\"id\":66417,"
    "\"created_at\":\"2026-07-20T01:22:33.327Z\",\"sender\":\"+15551234567\","
    "\"chat_guid\":\"any;-;+15551234567\",\"reactions\":[],\"attachments\":[]}";

static const char *FIXTURE_REACTION_ROW =
    "{\"is_reaction\":true,\"is_reaction_add\":true,\"reaction_type\":\"love\","
    "\"reacted_to_guid\":\"91136D96-D7A1-40E3-BD14-092A3EA618FC\",\"chat_id\":16,"
    "\"id\":66416,\"text\":\"Loved probe A\",\"is_from_me\":true}";

static bool parse_fixture(const char *line, hu_imessage_bb_event_t *out) {
    hu_allocator_t alloc = hu_system_allocator();
    return hu_imessage_bb_event_parse(&alloc, line, strlen(line), out);
}

/* ── Parse: the three real event kinds ─────────────────────────────────── */

static void bb_parse_typing_start_extracts_chat_and_handle(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_TRUE(parse_fixture(FIXTURE_TYPING_START, &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_TYPING_START);
    HU_ASSERT_STR_EQ(ev.chat_guid, "iMessage;-;+15551234567");
    HU_ASSERT_STR_EQ(ev.handle, "+15551234567");
    HU_ASSERT_TRUE(ev.timestamp > 1784510000.0 && ev.timestamp < 1784510001.0);
}

/* Key order differs from the fixture above — same parse must result. */
static void bb_parse_is_key_based_not_order_based(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_TRUE(parse_fixture(FIXTURE_TYPING_STOP, &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_TYPING_STOP);
    HU_ASSERT_STR_EQ(ev.chat_guid, "iMessage;-;+15551234567");
    HU_ASSERT_STR_EQ(ev.handle, "+15551234567");
}

/* "stopped-typing" contains "typing" and shares a prefix family with
 * "started-typing" — a substring classifier would collapse them.
 * (~/.claude/rules/substring-classifier-pitfalls.md) */
static void bb_parse_start_and_stop_are_distinct_kinds(void) {
    hu_imessage_bb_event_t start, stop;
    HU_ASSERT_TRUE(parse_fixture(FIXTURE_TYPING_START, &start));
    HU_ASSERT_TRUE(parse_fixture(FIXTURE_TYPING_STOP, &stop));
    HU_ASSERT_TRUE(start.kind != stop.kind);
    HU_ASSERT_EQ((int)start.kind, (int)HU_IMSG_BB_TYPING_START);
    HU_ASSERT_EQ((int)stop.kind, (int)HU_IMSG_BB_TYPING_STOP);
}

static void bb_parse_aliases_removed(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_TRUE(parse_fixture(FIXTURE_ALIASES_REMOVED, &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_ALIASES_REMOVED);
}

/* The data-key names are INFERRED from the dylib string table, not observed
 * from real inbound traffic (schema doc §5). If the guess is wrong we must
 * still see the event rather than silently drop it. */
static void bb_parse_missing_data_keys_still_yields_event(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_TRUE(parse_fixture(FIXTURE_EMPTY_DATA, &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_TYPING_START);
    HU_ASSERT_STR_EQ(ev.chat_guid, "");
    HU_ASSERT_STR_EQ(ev.handle, "");
}

static void bb_parse_unknown_event_name_is_bridge_but_unknown(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_TRUE(parse_fixture(FIXTURE_UNKNOWN_EVENT, &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_UNKNOWN);
}

/* ── Discrimination: the other traffic on this stream ──────────────────── */

static void bb_parse_chatdb_message_row_is_not_a_bridge_event(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_FALSE(parse_fixture(FIXTURE_MESSAGE_ROW, &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_NONE);
}

static void bb_parse_reaction_row_is_not_a_bridge_event(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_FALSE(parse_fixture(FIXTURE_REACTION_ROW, &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_NONE);
}

/* ── Fail closed ───────────────────────────────────────────────────────── */

static void bb_parse_garbage_and_empty_fail_closed(void) {
    hu_imessage_bb_event_t ev;
    HU_ASSERT_FALSE(parse_fixture("", &ev));
    HU_ASSERT_FALSE(parse_fixture("not json at all", &ev));
    HU_ASSERT_FALSE(parse_fixture("{", &ev));
    /* Truncated mid-line: the discriminator is present but the JSON is not
     * closed — a pipe read() boundary can produce exactly this. */
    HU_ASSERT_FALSE(parse_fixture("{\"kind\":\"bridge-event\",\"event\":\"star", &ev));
    HU_ASSERT_EQ((int)ev.kind, (int)HU_IMSG_BB_NONE);
}

static void bb_parse_null_args_fail_closed(void) {
    hu_imessage_bb_event_t ev;
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_FALSE(hu_imessage_bb_event_parse(&alloc, NULL, 10, &ev));
    HU_ASSERT_FALSE(
        hu_imessage_bb_event_parse(NULL, FIXTURE_TYPING_START, strlen(FIXTURE_TYPING_START), &ev));
    HU_ASSERT_FALSE(hu_imessage_bb_event_parse(&alloc, FIXTURE_TYPING_START,
                                               strlen(FIXTURE_TYPING_START), NULL));
}

/* A hostile/garbled chat guid must not overflow the fixed field. */
static void bb_parse_overlong_ids_are_bounded_and_terminated(void) {
    char line[HU_IMSG_BB_ID_CAP * 4];
    char big[HU_IMSG_BB_ID_CAP * 2];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    snprintf(line, sizeof(line),
             "{\"kind\":\"bridge-event\",\"event\":\"started-typing\","
             "\"data\":{\"chatGuid\":\"%s\"}}",
             big);

    hu_imessage_bb_event_t ev;
    HU_ASSERT_TRUE(parse_fixture(line, &ev));
    HU_ASSERT_TRUE(strlen(ev.chat_guid) == HU_IMSG_BB_ID_CAP - 1);
}

/* ── Activation gate ───────────────────────────────────────────────────── */

static void bb_mode_defaults_to_off_and_fails_closed(void) {
    HU_ASSERT_EQ((int)hu_imessage_bb_mode_from_env(NULL), (int)HU_IMSG_BB_MODE_OFF);
    HU_ASSERT_EQ((int)hu_imessage_bb_mode_from_env(""), (int)HU_IMSG_BB_MODE_OFF);
    HU_ASSERT_EQ((int)hu_imessage_bb_mode_from_env("off"), (int)HU_IMSG_BB_MODE_OFF);
    /* Typos must degrade to OFF, never silently arm the subsystem. */
    HU_ASSERT_EQ((int)hu_imessage_bb_mode_from_env("shadwo"), (int)HU_IMSG_BB_MODE_OFF);
    HU_ASSERT_EQ((int)hu_imessage_bb_mode_from_env("LIVE"), (int)HU_IMSG_BB_MODE_OFF);
}

static void bb_mode_parses_shadow_and_live(void) {
    HU_ASSERT_EQ((int)hu_imessage_bb_mode_from_env("shadow"), (int)HU_IMSG_BB_MODE_SHADOW);
    HU_ASSERT_EQ((int)hu_imessage_bb_mode_from_env("live"), (int)HU_IMSG_BB_MODE_LIVE);
}

/* ── Send-hold policy ──────────────────────────────────────────────────── */

static void bb_hold_requires_an_observed_typing_start(void) {
    double hold = -1.0;
    /* No event seen at all — absence of evidence must not hold a send. */
    HU_ASSERT_FALSE(hu_imessage_bb_should_hold_send(HU_IMSG_BB_NONE, 0.0, 1000.0, &hold));
    HU_ASSERT_TRUE(hold == 0.0);
    /* They stopped typing — nothing to wait for. */
    HU_ASSERT_FALSE(hu_imessage_bb_should_hold_send(HU_IMSG_BB_TYPING_STOP, 999.0, 1000.0, &hold));
    /* An unrelated event kind never holds. */
    HU_ASSERT_FALSE(
        hu_imessage_bb_should_hold_send(HU_IMSG_BB_ALIASES_REMOVED, 999.0, 1000.0, &hold));
}

static void bb_hold_ignores_future_timestamps(void) {
    double hold = -1.0;
    /* Clock skew / replay: a start stamped in the future is not evidence. */
    HU_ASSERT_FALSE(
        hu_imessage_bb_should_hold_send(HU_IMSG_BB_TYPING_START, 2000.0, 1000.0, &hold));
    HU_ASSERT_TRUE(hold == 0.0);
}

static void bb_hold_holds_while_contact_is_mid_sentence(void) {
    double hold = 0.0;
    /* One second after they started typing, we are plainly mid-sentence. */
    HU_ASSERT_TRUE(hu_imessage_bb_should_hold_send(HU_IMSG_BB_TYPING_START, 1000.0, 1001.0, &hold));
    HU_ASSERT_TRUE(hold > 0.0);
}

/* iMessage emits NO stop event when a draft is simply abandoned, so a single
 * typing-START must not suppress sends forever. */
static void bb_hold_expires_on_abandoned_draft(void) {
    double hold = -1.0;
    HU_ASSERT_FALSE(
        hu_imessage_bb_should_hold_send(HU_IMSG_BB_TYPING_START, 1000.0, 1000.0 + 600.0, &hold));
    HU_ASSERT_TRUE(hold == 0.0);
}

/* The ceiling is anchored on Apple's own ~60s typing-indicator expiry, not on
 * taste — past it the SENDER's device has stopped advertising typing, so the
 * signal we would be holding on is one Apple itself retired. Pin the boundary
 * so it cannot drift silently. */
static void bb_hold_ceiling_tracks_apple_indicator_expiry(void) {
    double hold = 0.0;
    /* Just inside: a slow composer is still protected. */
    HU_ASSERT_TRUE(hu_imessage_bb_hold_policy(59.0, &hold));
    HU_ASSERT_TRUE(hold > 0.0);
    /* At/after the expiry: the indicator is stale, stop holding. */
    HU_ASSERT_FALSE(hu_imessage_bb_hold_policy(60.0, &hold));
    HU_ASSERT_TRUE(hold == 0.0);
    HU_ASSERT_FALSE(hu_imessage_bb_hold_policy(61.0, &hold));
}

/* Never commit to one long sleep: a stopped-typing event can land at any
 * moment, so each hold must be short enough to re-evaluate against it. */
static void bb_hold_recheck_interval_is_bounded(void) {
    double hold = 0.0;
    HU_ASSERT_TRUE(hu_imessage_bb_hold_policy(0.0, &hold));
    HU_ASSERT_TRUE(hold > 0.0 && hold <= 2.0);
    /* Near the ceiling the wait shrinks to the remaining time, never past it. */
    HU_ASSERT_TRUE(hu_imessage_bb_hold_policy(59.5, &hold));
    HU_ASSERT_TRUE(hold > 0.0 && hold <= 0.5 + 1e-9);
}

/* NULL out-param must be safe — the predicate is called for logging in SHADOW
 * where the caller may not care how long it would have held. */
static void bb_hold_policy_tolerates_null_out(void) {
    HU_ASSERT_TRUE(hu_imessage_bb_hold_policy(1.0, NULL));
    HU_ASSERT_FALSE(hu_imessage_bb_hold_policy(600.0, NULL));
}

/* ── E2E replay: real bytes, real framing ──────────────────────────────
 * tests/fixtures/bb-events/live-capture.jsonl is stdout captured VERBATIM from
 * a live `imsg watch --json --since-rowid N --bb-events` with the IMCore
 * bridge connected (regenerate: bash scripts/e2e-bb-events.sh --write-fixture).
 * Replaying it here proves the production framing + parser against genuine CLI
 * output rather than hand-written strings, and keeps that proof running in CI
 * on hosts with no bridge at all. */

typedef struct replay_counts {
    int typing_start;
    int typing_stop;
    int aliases_removed;
    int unknown;
    int total;
    char last_chat[HU_IMSG_BB_ID_CAP];
} replay_counts_t;

static void replay_on_event(const hu_imessage_bb_event_t *ev, void *user) {
    replay_counts_t *c = (replay_counts_t *)user;
    c->total++;
    switch (ev->kind) {
    case HU_IMSG_BB_TYPING_START:
        c->typing_start++;
        break;
    case HU_IMSG_BB_TYPING_STOP:
        c->typing_stop++;
        break;
    case HU_IMSG_BB_ALIASES_REMOVED:
        c->aliases_removed++;
        break;
    default:
        c->unknown++;
        break;
    }
    snprintf(c->last_chat, sizeof(c->last_chat), "%s", ev->chat_guid);
}

/* Returns bytes read, or 0 when the fixture is unavailable. */
static size_t load_live_capture(char *buf, size_t cap) {
    /* Tests may run from the repo root or from build/; try both. */
    static const char *paths[] = {
        "tests/fixtures/bb-events/live-capture.jsonl",
        "../tests/fixtures/bb-events/live-capture.jsonl",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        size_t n = fread(buf, 1, cap - 1, f);
        fclose(f);
        buf[n] = '\0';
        return n;
    }
    return 0;
}

static void bb_stream_replays_live_capture(void) {
    char buf[8192];
    size_t n = load_live_capture(buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0); /* fixture must be present — it is committed */

    hu_allocator_t alloc = hu_system_allocator();
    hu_imessage_bb_stream_t st;
    memset(&st, 0, sizeof(st));
    replay_counts_t counts;
    memset(&counts, 0, sizeof(counts));

    hu_imessage_bb_stream_consume(&st, &alloc, buf, n, replay_on_event, &counts);

    HU_ASSERT_EQ(counts.total, 3);
    HU_ASSERT_EQ(counts.typing_start, 1);
    HU_ASSERT_EQ(counts.typing_stop, 1);
    HU_ASSERT_EQ(counts.aliases_removed, 1);
    HU_ASSERT_EQ(counts.unknown, 0);
}

/* The production reader is read(2) on a pipe: records arrive split at
 * arbitrary offsets, including mid-JSON and mid-UTF8. Replay the SAME real
 * bytes at hostile chunk sizes — byte-at-a-time is the worst case. */
static void bb_stream_reassembles_across_chunk_boundaries(void) {
    char buf[8192];
    size_t n = load_live_capture(buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);

    const size_t chunk_sizes[] = {1, 2, 7, 64, 137, 4096};
    hu_allocator_t alloc = hu_system_allocator();

    for (size_t c = 0; c < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); c++) {
        size_t step = chunk_sizes[c];
        hu_imessage_bb_stream_t st;
        memset(&st, 0, sizeof(st));
        replay_counts_t counts;
        memset(&counts, 0, sizeof(counts));

        for (size_t off = 0; off < n; off += step) {
            size_t len = (off + step > n) ? (n - off) : step;
            hu_imessage_bb_stream_consume(&st, &alloc, buf + off, len, replay_on_event, &counts);
        }
        /* Identical results regardless of how the stream was diced. */
        HU_ASSERT_EQ(counts.total, 3);
        HU_ASSERT_EQ(counts.typing_start, 1);
        HU_ASSERT_EQ(counts.typing_stop, 1);
    }
}

/* A message row and a bridge event on the same stream must be separated. */
static void bb_stream_ignores_interleaved_message_rows(void) {
    char stream[4096];
    snprintf(stream, sizeof(stream), "%s\n%s\n%s\n", FIXTURE_MESSAGE_ROW, FIXTURE_TYPING_START,
             FIXTURE_REACTION_ROW);

    hu_allocator_t alloc = hu_system_allocator();
    hu_imessage_bb_stream_t st;
    memset(&st, 0, sizeof(st));
    replay_counts_t counts;
    memset(&counts, 0, sizeof(counts));

    hu_imessage_bb_stream_consume(&st, &alloc, stream, strlen(stream), replay_on_event, &counts);

    /* Exactly one bridge event; the two chat.db rows are not events. */
    HU_ASSERT_EQ(counts.total, 1);
    HU_ASSERT_EQ(counts.typing_start, 1);
    HU_ASSERT_STR_EQ(counts.last_chat, "iMessage;-;+15551234567");
}

/* An over-long line must be dropped whole, never truncated into half-JSON that
 * could parse as a plausible-but-wrong event — and the stream must resync on
 * the next newline. */
static void bb_stream_drops_overlong_line_and_resyncs(void) {
    char stream[16384];
    size_t pos = 0;
    for (; pos < 6000; pos++)
        stream[pos] = 'x';
    stream[pos++] = '\n';
    int written = snprintf(stream + pos, sizeof(stream) - pos, "%s\n", FIXTURE_TYPING_START);
    HU_ASSERT_TRUE(written > 0);
    size_t total = pos + (size_t)written;

    hu_allocator_t alloc = hu_system_allocator();
    hu_imessage_bb_stream_t st;
    memset(&st, 0, sizeof(st));
    replay_counts_t counts;
    memset(&counts, 0, sizeof(counts));

    hu_imessage_bb_stream_consume(&st, &alloc, stream, total, replay_on_event, &counts);

    HU_ASSERT_EQ(counts.total, 1);
    HU_ASSERT_EQ(counts.typing_start, 1);
}

void run_imessage_bb_event_tests(void) {
    HU_TEST_SUITE("imessage_bb_event");
    HU_RUN_TEST(bb_parse_typing_start_extracts_chat_and_handle);
    HU_RUN_TEST(bb_parse_is_key_based_not_order_based);
    HU_RUN_TEST(bb_parse_start_and_stop_are_distinct_kinds);
    HU_RUN_TEST(bb_parse_aliases_removed);
    HU_RUN_TEST(bb_parse_missing_data_keys_still_yields_event);
    HU_RUN_TEST(bb_parse_unknown_event_name_is_bridge_but_unknown);
    HU_RUN_TEST(bb_parse_chatdb_message_row_is_not_a_bridge_event);
    HU_RUN_TEST(bb_parse_reaction_row_is_not_a_bridge_event);
    HU_RUN_TEST(bb_parse_garbage_and_empty_fail_closed);
    HU_RUN_TEST(bb_parse_null_args_fail_closed);
    HU_RUN_TEST(bb_parse_overlong_ids_are_bounded_and_terminated);
    HU_RUN_TEST(bb_mode_defaults_to_off_and_fails_closed);
    HU_RUN_TEST(bb_mode_parses_shadow_and_live);
    HU_RUN_TEST(bb_hold_requires_an_observed_typing_start);
    HU_RUN_TEST(bb_hold_ignores_future_timestamps);
    HU_RUN_TEST(bb_hold_holds_while_contact_is_mid_sentence);
    HU_RUN_TEST(bb_hold_expires_on_abandoned_draft);
    HU_RUN_TEST(bb_hold_ceiling_tracks_apple_indicator_expiry);
    HU_RUN_TEST(bb_hold_recheck_interval_is_bounded);
    HU_RUN_TEST(bb_hold_policy_tolerates_null_out);
    HU_RUN_TEST(bb_stream_replays_live_capture);
    HU_RUN_TEST(bb_stream_reassembles_across_chunk_boundaries);
    HU_RUN_TEST(bb_stream_ignores_interleaved_message_rows);
    HU_RUN_TEST(bb_stream_drops_overlong_line_and_resyncs);
}
