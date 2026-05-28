/* tests/test_reflection_retire_on_contradiction.c — Phase 2 (T8) of
 * docs/plans/2026-05-26-reflection-loop.
 *
 * Retire reflection patterns when the user thumbs-downs a turn that
 * surfaced them. Pins the lineage contract:
 *
 *   AC-1  thumbs_down on a turn whose system prompt surfaced reflection
 *         patterns (in that channel, within the contradiction window)
 *         → those patterns get retired=1.
 *   AC-2  the T2 storage UPSERT preserves `retired` across re-derivation
 *         (a later reflection run that re-derives the same pattern does
 *         NOT un-retire it).
 *   AC-3  a surfacing OLDER than HU_REFLECTION_CONTRADICTION_WINDOW_MS is
 *         NOT retired by a later thumbs_down (window scoping).
 *   AC-4  hu_reflection_note_surfaced is idempotent on (pattern, channel).
 *   AC-5  a POSITIVE reaction does NOT retire surfaced patterns.
 *
 * The full-flow tests drive the real production path:
 *   insert pattern → hu_personal_model_build_prompt_with_reflection
 *   (records the surfacing) → hu_reaction_handler_set_reflection_db →
 *   hu_reaction_handler_handle_event (NEGATIVE) → verify retired. */

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/memory/personal_model.h"
#include "human/reflection.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* ── Fixtures ──────────────────────────────────────────────────── */

static void insert_pattern(sqlite3 *db, const char *id, const char *observation,
                           const char *channels_json) {
    sqlite3_stmt *st = NULL;
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    const char *sql =
        "INSERT INTO reflection_patterns (id, type, subject, observation, confidence, "
        "evidence_json, channels_json, first_seen_run_id, last_seen_run_id, "
        "observation_count, created_at_ms, last_observed_at_ms, expires_at_ms, "
        "surfaced_to_user, retired) "
        "VALUES (?, 'preference', 'Seth', ?, 0.85, '[]', ?, 'run_seed', 'run_seed', 1, "
        "?, ?, ?, 0, 0)";
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, observation, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, channels_json, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now_ms);                         /* created */
    sqlite3_bind_int64(st, 5, (sqlite3_int64)now_ms);                         /* observed */
    sqlite3_bind_int64(st, 6, (sqlite3_int64)(now_ms + 30ULL * 86400000ULL)); /* expires */
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static int pattern_is_retired(sqlite3 *db, const char *id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT retired FROM reflection_patterns WHERE id = ?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    int retired = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        retired = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return retired;
}

static int surfacing_row_count(sqlite3 *db, const char *channel) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM reflection_surfacings WHERE channel = ?", -1, &st,
                       NULL);
    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static hu_personal_model_t empty_model(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.core.user_name, sizeof m.core.user_name, "%s", "Seth");
    return m;
}

static hu_reaction_event_t make_reaction(const char *channel, hu_reaction_polarity_t polarity,
                                         hu_reaction_kind_t kind) {
    hu_reaction_event_t e = {0};
    e.channel_id = channel;
    e.target_thread_id = "chat-guid-1";
    e.target_message_ref = "msg-ref-1";
    e.sender_handle = "seth";
    e.kind = kind;
    e.polarity = polarity;
    e.timestamp_unix = (int64_t)time(NULL);
    e.is_removal = 0;
    return e;
}

/* ── AC-1: thumbs_down retires a surfaced pattern (full flow) ───── */

static void test_thumbs_down_retires_surfaced_pattern(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_pattern(db, "p1", "prefers jazz between 3-5pm", "[\"imessage\"]");

    /* Surface the pattern via the real personal-model build path; this
     * records the (p1, imessage, now) lineage in reflection_surfacings. */
    hu_personal_model_t m = empty_model();
    char buf[8192];
    hu_personal_model_build_prompt_with_reflection(&m, NULL, db, "imessage", 5, buf, sizeof buf);
    HU_ASSERT(strstr(buf, "prefers jazz between 3-5pm") != NULL);
    HU_ASSERT_EQ(surfacing_row_count(db, "imessage"), 1);
    HU_ASSERT_EQ(pattern_is_retired(db, "p1"), 0);

    /* User thumbs-downs the turn. */
    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_reflection_db(db);
    hu_reaction_event_t e = make_reaction("imessage", HU_REACTION_NEGATIVE, HU_REACTION_DISLIKE);
    (void)hu_reaction_handler_handle_event(&e);

    HU_ASSERT_EQ(pattern_is_retired(db, "p1"), 1);
    /* Surfacings consumed so a later unrelated thumbs_down can't re-fire. */
    HU_ASSERT_EQ(surfacing_row_count(db, "imessage"), 0);

    hu_reaction_handler_reset_for_test();
    sqlite3_close(db);
}

/* ── AC-2: retired survives re-derivation through the T2 UPSERT ─── */

static void test_retired_preserved_across_rederivation(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_pattern(db, "p1", "prefers jazz between 3-5pm", "[\"imessage\"]");

    /* Retire it directly (simulating a prior contradiction). */
    hu_reflection_retire(db, "p1");
    HU_ASSERT_EQ(pattern_is_retired(db, "p1"), 1);

    /* A later reflection run re-derives the SAME pattern (same id) and
     * UPSERTs it. The retired flag MUST be preserved. */
    hu_reflection_pattern_t p = {0};
    snprintf(p.id, sizeof p.id, "%s", "p1");
    p.type = HU_REFLECTION_PATTERN_PREFERENCE;
    snprintf(p.subject, sizeof p.subject, "%s", "Seth");
    snprintf(p.observation, sizeof p.observation, "%s", "prefers jazz between 3-5pm");
    p.confidence = 0.9;
    snprintf(p.channels[0], sizeof p.channels[0], "%s", "imessage");
    p.channel_count = 1;
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    p.created_at_ms = now_ms;
    p.last_observed_at_ms = now_ms;
    p.expires_at_ms = now_ms + 30ULL * 86400000ULL;
    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_2", &p), HU_OK);

    HU_ASSERT_EQ(pattern_is_retired(db, "p1"), 1);
    sqlite3_close(db);
}

/* ── AC-3: a surfacing older than the window is not retired ─────── */

static void test_out_of_window_surfacing_not_retired(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_pattern(db, "p1", "prefers jazz between 3-5pm", "[\"imessage\"]");

    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    /* Surfaced well outside the contradiction window (2x window ago). */
    uint64_t old_ms = now_ms - 2ULL * HU_REFLECTION_CONTRADICTION_WINDOW_MS;
    hu_reflection_note_surfaced(db, "p1", "imessage", old_ms);

    int retired = hu_reflection_retire_contradicted(db, "imessage",
                                                    HU_REFLECTION_CONTRADICTION_WINDOW_MS, now_ms);
    HU_ASSERT_EQ(retired, 0);
    HU_ASSERT_EQ(pattern_is_retired(db, "p1"), 0);
    sqlite3_close(db);
}

/* ── AC-4: note_surfaced is idempotent on (pattern, channel) ────── */

static void test_note_surfaced_idempotent(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);

    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    hu_reflection_note_surfaced(db, "p1", "imessage", now_ms);
    hu_reflection_note_surfaced(db, "p1", "imessage", now_ms + 1000);
    HU_ASSERT_EQ(surfacing_row_count(db, "imessage"), 1);

    /* Different channel is a distinct row. */
    hu_reflection_note_surfaced(db, "p1", "slack", now_ms);
    HU_ASSERT_EQ(surfacing_row_count(db, "slack"), 1);
    sqlite3_close(db);
}

/* ── AC-5: a positive reaction does NOT retire ─────────────────── */

static void test_positive_reaction_does_not_retire(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_pattern(db, "p1", "prefers jazz between 3-5pm", "[\"imessage\"]");

    hu_personal_model_t m = empty_model();
    char buf[8192];
    hu_personal_model_build_prompt_with_reflection(&m, NULL, db, "imessage", 5, buf, sizeof buf);
    HU_ASSERT_EQ(surfacing_row_count(db, "imessage"), 1);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_reflection_db(db);
    hu_reaction_event_t e = make_reaction("imessage", HU_REACTION_POSITIVE, HU_REACTION_LIKE);
    (void)hu_reaction_handler_handle_event(&e);

    HU_ASSERT_EQ(pattern_is_retired(db, "p1"), 0);
    /* Positive reaction leaves the lineage intact (only NEGATIVE consumes). */
    HU_ASSERT_EQ(surfacing_row_count(db, "imessage"), 1);

    hu_reaction_handler_reset_for_test();
    sqlite3_close(db);
}

/* ── AC-1 variant: cross-channel scoping — thumbs_down in one channel
 *    does not retire a pattern surfaced only in another channel. ─── */

static void test_thumbs_down_is_channel_scoped(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_pattern(db, "p1", "single-channel slack pattern", "[\"slack\"]");

    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    hu_reflection_note_surfaced(db, "p1", "slack", now_ms);

    /* thumbs_down arrives on imessage — must NOT touch the slack pattern. */
    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_reflection_db(db);
    hu_reaction_event_t e = make_reaction("imessage", HU_REACTION_NEGATIVE, HU_REACTION_DISLIKE);
    (void)hu_reaction_handler_handle_event(&e);

    HU_ASSERT_EQ(pattern_is_retired(db, "p1"), 0);
    HU_ASSERT_EQ(surfacing_row_count(db, "slack"), 1);

    hu_reaction_handler_reset_for_test();
    sqlite3_close(db);
}

/* ── AC-6: same-channel blast radius is INTENDED, and consuming the
 *    lineage prevents a later thumbs_down from re-retiring. ──────────
 *
 * Attribution is channel-scoped + recency-windowed, not msg_ref-precise
 * (see the schema comment in storage.c). A consequence: two patterns
 * surfaced from different turns in the same channel within the window are
 * BOTH retired by a single thumbs_down. This test pins that as the
 * intended contract — if a future change makes attribution precise, this
 * test should be updated deliberately, not silently broken. It also pins
 * that a second thumbs_down does NOT re-retire (the lineage was consumed,
 * and the retired=0 guard would block it regardless). */

static void test_same_channel_thumbs_down_retires_all_in_window(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_pattern(db, "pA", "pattern from turn A", "[\"imessage\"]");
    insert_pattern(db, "pB", "pattern from turn B", "[\"imessage\"]");

    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    /* Two distinct turns, both inside the contradiction window. */
    hu_reflection_note_surfaced(db, "pA", "imessage", now_ms - 60000); /* 1 min ago */
    hu_reflection_note_surfaced(db, "pB", "imessage", now_ms - 1000);  /* 1 sec ago */
    HU_ASSERT_EQ(surfacing_row_count(db, "imessage"), 2);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_reflection_db(db);
    hu_reaction_event_t e = make_reaction("imessage", HU_REACTION_NEGATIVE, HU_REACTION_DISLIKE);
    (void)hu_reaction_handler_handle_event(&e);

    /* Documented blast radius: BOTH in-window patterns retire. */
    HU_ASSERT_EQ(pattern_is_retired(db, "pA"), 1);
    HU_ASSERT_EQ(pattern_is_retired(db, "pB"), 1);
    /* Lineage consumed. */
    HU_ASSERT_EQ(surfacing_row_count(db, "imessage"), 0);

    /* A second thumbs_down has nothing left to retire (count == 0) and
     * cannot re-retire the already-retired patterns. */
    int retired_again = hu_reflection_retire_contradicted(
        db, "imessage", HU_REFLECTION_CONTRADICTION_WINDOW_MS, now_ms);
    HU_ASSERT_EQ(retired_again, 0);
    HU_ASSERT_EQ(pattern_is_retired(db, "pA"), 1);
    HU_ASSERT_EQ(pattern_is_retired(db, "pB"), 1);

    hu_reaction_handler_reset_for_test();
    sqlite3_close(db);
}

void run_reflection_retire_on_contradiction_tests(void) {
    HU_TEST_SUITE("reflection_retire_on_contradiction");
    HU_RUN_TEST(test_thumbs_down_retires_surfaced_pattern);
    HU_RUN_TEST(test_retired_preserved_across_rederivation);
    HU_RUN_TEST(test_out_of_window_surfacing_not_retired);
    HU_RUN_TEST(test_note_surfaced_idempotent);
    HU_RUN_TEST(test_positive_reaction_does_not_retire);
    HU_RUN_TEST(test_thumbs_down_is_channel_scoped);
    HU_RUN_TEST(test_same_channel_thumbs_down_retires_all_in_window);
}

#else /* !HU_ENABLE_SQLITE */

void run_reflection_retire_on_contradiction_tests(void) {
    /* Stub when SQLite is disabled — keeps the runner symbol resolvable
     * so this file can stay in the unconditional HU_TEST_SOURCES list. */
}

#endif /* HU_ENABLE_SQLITE */
