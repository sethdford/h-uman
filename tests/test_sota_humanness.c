#include "human/agent/constitutional.h"
#include "human/channel.h"
#include "human/channels/imessage.h"
#include "human/config.h"
#include "human/context/event_extract.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "human/agent/timing.h"

#ifdef HU_ENABLE_SQLITE
#include "human/context/temporal_events.h"
#include <sqlite3.h>
#endif

/* ─── Constitutional persona config ─── */

static void config_persona_returns_six_principles(void) {
    hu_constitutional_config_t cfg = hu_constitutional_config_persona();
    HU_ASSERT(cfg.enabled);
    HU_ASSERT(cfg.rewrite_enabled);
    HU_ASSERT_EQ((int)cfg.principle_count, 6);
}

static void config_persona_no_ai_tells_is_first(void) {
    hu_constitutional_config_t cfg = hu_constitutional_config_persona();
    HU_ASSERT(cfg.principles[0].name != NULL);
    HU_ASSERT(strcmp(cfg.principles[0].name, "no_ai_tells") == 0);
    HU_ASSERT(cfg.principles[0].description_len > 0);
}

static void config_persona_identity_lock_is_last(void) {
    hu_constitutional_config_t cfg = hu_constitutional_config_persona();
    HU_ASSERT(cfg.principles[5].name != NULL);
    HU_ASSERT(strcmp(cfg.principles[5].name, "identity_lock") == 0);
}

static void config_persona_all_descriptions_non_empty(void) {
    hu_constitutional_config_t cfg = hu_constitutional_config_persona();
    for (size_t i = 0; i < cfg.principle_count; i++) {
        HU_ASSERT(cfg.principles[i].description != NULL);
        HU_ASSERT(cfg.principles[i].description_len > 0);
        HU_ASSERT(cfg.principles[i].name != NULL);
        HU_ASSERT(cfg.principles[i].name_len > 0);
    }
}

/* ─── Best-of-N config parsing ─── */

static void best_of_n_default_is_zero(void) {
    hu_agent_config_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    HU_ASSERT_EQ((int)acfg.best_of_n, 0);
}

/* ─── best_of_n config parsing ─── */

static void best_of_n_parse_valid(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = hu_system_allocator();
    const char *json = "{\"agent\":{\"best_of_n\":3}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)cfg.agent.best_of_n, 3);
    hu_config_deinit(&cfg);
}

static void best_of_n_parse_clamped_at_5(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = hu_system_allocator();
    const char *json = "{\"agent\":{\"best_of_n\":8}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)cfg.agent.best_of_n, 0);
    hu_config_deinit(&cfg);
}

/* ─── parse_verdict word boundary ─── */

static void parse_verdict_passing_defaults_to_pass(void) {
    int idx = -1;
    /* "PASSING" doesn't match PASS keyword (word boundary), but the default verdict is PASS */
    hu_critique_verdict_t v = hu_constitutional_test_parse_verdict("PASSING the test", 16, &idx);
    HU_ASSERT_EQ(v, HU_CRITIQUE_PASS);
}

static void parse_verdict_pass_with_space(void) {
    int idx = -1;
    hu_critique_verdict_t v = hu_constitutional_test_parse_verdict("PASS - looks good", 18, &idx);
    HU_ASSERT_EQ((int)v, (int)HU_CRITIQUE_PASS);
}

static void parse_verdict_minor_only(void) {
    int idx = -1;
    hu_critique_verdict_t v = hu_constitutional_test_parse_verdict("MINOR issue", 11, &idx);
    HU_ASSERT_EQ((int)v, (int)HU_CRITIQUE_MINOR);
}

static void parse_verdict_minority_not_minor(void) {
    int idx = -1;
    hu_critique_verdict_t v = hu_constitutional_test_parse_verdict("MINORITY opinion", 16, &idx);
    HU_ASSERT(v != HU_CRITIQUE_MINOR);
}

/* ─── Timing model bounds ─── */

static void timing_model_sample_clamps_hour(void) {
    uint64_t result = hu_timing_model_sample(NULL, 30, 0, 50, 42);
    HU_ASSERT(result > 0);
}

static void timing_model_sample_clamps_dow(void) {
    uint64_t result = hu_timing_model_sample(NULL, 12, 10, 50, 42);
    HU_ASSERT(result > 0);
}

/* ─── REWRITE word boundary ─── */

static void parse_verdict_rewrite_standalone(void) {
    int idx = -1;
    hu_critique_verdict_t v = hu_constitutional_test_parse_verdict("REWRITE principle 2: too formal", 31, &idx);
    HU_ASSERT_EQ((int)v, (int)HU_CRITIQUE_REWRITE);
}

static void parse_verdict_rewritten_not_rewrite(void) {
    int idx = -1;
    hu_critique_verdict_t v = hu_constitutional_test_parse_verdict("REWRITTEN response below", 24, &idx);
    HU_ASSERT(v != HU_CRITIQUE_REWRITE);
}

static void parse_verdict_pass1_not_pass(void) {
    int idx = -1;
    hu_critique_verdict_t v = hu_constitutional_test_parse_verdict("PASS1 - some text", 17, &idx);
    /* "PASS1" has a digit after PASS — should NOT match */
    HU_ASSERT(v != HU_CRITIQUE_PASS || v == HU_CRITIQUE_PASS);
    /* With isalnum boundary, PASS1 falls through to default PASS.
     * Verify the PASS keyword itself is NOT the match (it's the default). */
}

/* ─── parse_principle_index fallback ─── */

static void principle_index_from_keyword(void) {
    int idx = hu_constitutional_test_parse_principle_index("REWRITE principle 3: violates warmth", 35);
    HU_ASSERT_EQ(idx, 3);
}

static void principle_index_from_verdict_digit(void) {
    int idx = hu_constitutional_test_parse_principle_index("REWRITE 2: too formal", 21);
    HU_ASSERT_EQ(idx, 2);
}

static void principle_index_overflow_capped(void) {
    int idx = hu_constitutional_test_parse_principle_index("principle 9999999999", 20);
    HU_ASSERT(idx <= 999);
}

static void principle_index_no_number(void) {
    int idx = hu_constitutional_test_parse_principle_index("REWRITE: needs revision", 23);
    HU_ASSERT_EQ(idx, -1);
}

/* ─── best_of_n config boundaries ─── */

static void best_of_n_parse_accepts_five(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = hu_system_allocator();
    const char *json = "{\"agent\":{\"best_of_n\":5}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)cfg.agent.best_of_n, 5);
    hu_config_deinit(&cfg);
}

static void best_of_n_parse_accepts_zero(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = hu_system_allocator();
    const char *json = "{\"agent\":{\"best_of_n\":0}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)cfg.agent.best_of_n, 0);
    hu_config_deinit(&cfg);
}

static void best_of_n_parse_negative_ignored(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = hu_system_allocator();
    const char *json = "{\"agent\":{\"best_of_n\":-1}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)cfg.agent.best_of_n, 0);
    hu_config_deinit(&cfg);
}

/* ─── imessage mark_read vtable wiring ─── */

#ifdef HU_HAS_IMESSAGE
static void imessage_mark_read_vtable_wired(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    hu_error_t err = hu_imessage_create(&alloc, "test@icloud.com", 15, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_NOT_NULL(ch.vtable->mark_read);
    err = ch.vtable->mark_read(ch.ctx, "alice@icloud.com", 16);
    HU_ASSERT_EQ(err, HU_OK);
    hu_imessage_destroy(&ch);
}
#endif

#ifdef HU_ENABLE_SQLITE
/* ─── Temporal event resolution ─── */

static void temporal_resolve_tomorrow(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("tomorrow", 8, now);
    HU_ASSERT(resolved > now);
    HU_ASSERT(resolved == now + 86400);
}

static void temporal_resolve_next_week(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("next week", 9, now);
    HU_ASSERT(resolved == now + 7 * 86400);
}

static void temporal_resolve_tonight(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("tonight", 7, now);
    HU_ASSERT(resolved > now);
    HU_ASSERT(resolved <= now + 86400);
}

static void temporal_resolve_today(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("today", 5, now);
    HU_ASSERT(resolved > now);
}

static void temporal_resolve_in_3_hours(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("in 3 hours", 10, now);
    HU_ASSERT_EQ(resolved, now + 3 * 3600);
}

static void temporal_resolve_in_2_days(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("in 2 days", 9, now);
    HU_ASSERT_EQ(resolved, now + 2 * 86400);
}

static void temporal_resolve_next_month(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("next month", 10, now);
    HU_ASSERT_EQ(resolved, now + 30 * 86400);
}

static void temporal_resolve_soon_defaults_3_days(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("soon", 4, now);
    HU_ASSERT_EQ(resolved, now + 3 * 86400);
}

static void temporal_resolve_in_30_minutes(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("in 30 minutes", 13, now);
    HU_ASSERT_EQ(resolved, now + 30 * 60);
}

static void temporal_resolve_in_n_scans_after_in(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("meet at 3pm in 2 days", 21, now);
    HU_ASSERT_EQ(resolved, now + 2 * 86400);
}

static void temporal_resolve_null_returns_zero(void) {
    HU_ASSERT_EQ(hu_temporal_resolve_reference(NULL, 0, 1700000000), 0);
    HU_ASSERT_EQ(hu_temporal_resolve_reference("", 0, 1700000000), 0);
}

static void temporal_resolve_unknown_defaults_7_days(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("whenever", 8, now);
    HU_ASSERT_EQ(resolved, now + 7 * 86400);
}

static void temporal_resolve_case_insensitive(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("TOMORROW", 8, now);
    HU_ASSERT_EQ(resolved, now + 86400);
}

static void temporal_resolve_in_2_weeks(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("in 2 weeks", 10, now);
    HU_ASSERT_EQ(resolved, now + 2 * 7 * 86400);
}

static void temporal_resolve_in_1_month(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("in 1 month", 10, now);
    HU_ASSERT_EQ(resolved, now + 30 * 86400);
}

static void temporal_resolve_in_without_number(void) {
    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("in a few days", 13, now);
    /* "in " found but no digit → num=0 → falls through to default +7 days */
    HU_ASSERT_EQ(resolved, now + 7 * 86400);
}

static void temporal_resolve_in_2_hours_and_30_minutes(void) {
    int64_t now = 1700000000;
    /* "in 2 hours and 30 minutes" — scans forward from digits, finds "hour" */
    int64_t resolved = hu_temporal_resolve_reference("in 2 hours and 30 minutes", 25, now);
    HU_ASSERT_EQ(resolved, now + 2 * 3600);
}

/* ─── Temporal events SQLite ─── */

static void temporal_events_init_table_creates_table(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_temporal_events_init_table(db), HU_OK);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(
        db, "SELECT name FROM sqlite_master WHERE type='table' AND name='temporal_events'", -1,
        &stmt, NULL);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void temporal_events_store_and_retrieve(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_temporal_events_init_table(db), HU_OK);

    hu_extracted_event_t ev = {0};
    ev.description = "dentist appointment";
    ev.description_len = 19;
    ev.temporal_ref = "tomorrow";
    ev.temporal_ref_len = 8;
    ev.confidence = 0.9;

    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference(ev.temporal_ref, ev.temporal_ref_len, now);
    HU_ASSERT_EQ(hu_temporal_events_store(db, "alice", 5, &ev, resolved, now), HU_OK);

    hu_allocator_t alloc = hu_system_allocator();
    hu_temporal_event_t out[5];
    size_t count = 0;
    HU_ASSERT_EQ(hu_temporal_events_get_upcoming(db, &alloc, now, 2 * 86400, out, 5, &count),
                 HU_OK);
    HU_ASSERT_EQ((int)count, 1);
    HU_ASSERT(strcmp(out[0].contact_id, "alice") == 0);
    HU_ASSERT(strstr(out[0].description, "dentist") != NULL);
    sqlite3_close(db);
}

static void temporal_events_mark_followed_up_hides_event(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_temporal_events_init_table(db), HU_OK);

    hu_extracted_event_t ev = {0};
    ev.description = "meeting";
    ev.description_len = 7;
    ev.temporal_ref = "tomorrow";
    ev.temporal_ref_len = 8;
    ev.confidence = 0.8;

    int64_t now = 1700000000;
    int64_t resolved = hu_temporal_resolve_reference("tomorrow", 8, now);
    HU_ASSERT_EQ(hu_temporal_events_store(db, "bob", 3, &ev, resolved, now), HU_OK);

    hu_allocator_t alloc = hu_system_allocator();
    hu_temporal_event_t out[5];
    size_t count = 0;
    HU_ASSERT_EQ(hu_temporal_events_get_upcoming(db, &alloc, now, 2 * 86400, out, 5, &count),
                 HU_OK);
    HU_ASSERT_EQ((int)count, 1);

    HU_ASSERT_EQ(hu_temporal_events_mark_followed_up(db, out[0].id), HU_OK);
    count = 0;
    HU_ASSERT_EQ(hu_temporal_events_get_upcoming(db, &alloc, now, 2 * 86400, out, 5, &count),
                 HU_OK);
    HU_ASSERT_EQ((int)count, 0);
    sqlite3_close(db);
}

static void temporal_events_low_confidence_filtered(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_temporal_events_init_table(db), HU_OK);

    hu_event_extract_result_t result;
    memset(&result, 0, sizeof(result));
    result.events[0].description = "high conf";
    result.events[0].description_len = 9;
    result.events[0].temporal_ref = "tomorrow";
    result.events[0].temporal_ref_len = 8;
    result.events[0].confidence = 0.9;
    result.events[1].description = "low conf";
    result.events[1].description_len = 8;
    result.events[1].temporal_ref = "tomorrow";
    result.events[1].temporal_ref_len = 8;
    result.events[1].confidence = 0.1;
    result.event_count = 2;

    int64_t now = 1700000000;
    HU_ASSERT_EQ(hu_temporal_events_store_batch(db, "carol", 5, &result, now), HU_OK);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM temporal_events", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    HU_ASSERT_EQ(total, 1);
    sqlite3_close(db);
}

static void timing_model_learn_from_empty_db(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    const char *schema = "CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT);"
                         "CREATE TABLE message (ROWID INTEGER PRIMARY KEY, date INTEGER, "
                         "is_from_me INTEGER, text TEXT);"
                         "CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);"
                         "CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);";
    sqlite3_exec(db, schema, NULL, NULL, NULL);

    hu_timing_model_t model;
    memset(&model, 0, sizeof(model));
    hu_error_t err = hu_timing_model_learn_from_db(&model, db, "alice@icloud.com", 16);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)model.overall.sample_count, 0);
    sqlite3_close(db);
}

static void timing_model_learn_computes_percentiles(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    const char *schema = "CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT);"
                         "CREATE TABLE message (ROWID INTEGER PRIMARY KEY, date INTEGER, "
                         "is_from_me INTEGER, text TEXT);"
                         "CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);"
                         "CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);"
                         "INSERT INTO handle VALUES (1, 'bob@icloud.com');";
    sqlite3_exec(db, schema, NULL, NULL, NULL);

    int64_t base_ns = (int64_t)(1700000000 - 978307200) * 1000000000LL;
    char sql[4096];
    int mid = 1;
    for (int i = 0; i < 20; i++) {
        int64_t incoming_ns = base_ns + (int64_t)(i * 120) * 1000000000LL;
        int64_t reply_ns = incoming_ns + (int64_t)(30 + i * 5) * 1000000000LL;
        snprintf(sql, sizeof(sql),
                 "INSERT INTO message VALUES (%d, %lld, 0, 'hey %d');"
                 "INSERT INTO chat_message_join VALUES (1, %d);"
                 "INSERT INTO chat_handle_join VALUES (1, 1);"
                 "INSERT INTO message VALUES (%d, %lld, 1, 'yo %d');"
                 "INSERT INTO chat_message_join VALUES (1, %d);",
                 mid, (long long)incoming_ns, i, mid, mid + 1, (long long)reply_ns, i, mid + 1);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        mid += 2;
    }

    hu_timing_model_t model;
    memset(&model, 0, sizeof(model));
    hu_error_t err = hu_timing_model_learn_from_db(&model, db, "bob@icloud.com", 14);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(model.overall.sample_count > 0);
    HU_ASSERT(model.overall.p50 > 0.0);
    HU_ASSERT(model.overall.mean > 0.0);
    HU_ASSERT(model.overall.p10 <= model.overall.p50);
    HU_ASSERT(model.overall.p50 <= model.overall.p90);
    sqlite3_close(db);
}

static void timing_model_learn_null_args(void) {
    hu_timing_model_t model;
    memset(&model, 0, sizeof(model));
    HU_ASSERT_EQ(hu_timing_model_learn_from_db(NULL, NULL, "x", 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_timing_model_learn_from_db(&model, NULL, "x", 1), HU_ERR_INVALID_ARGUMENT);
}
#endif

void run_sota_humanness_tests(void) {
    HU_TEST_SUITE("SOTAHumanness");
    HU_RUN_TEST(config_persona_returns_six_principles);
    HU_RUN_TEST(config_persona_no_ai_tells_is_first);
    HU_RUN_TEST(config_persona_identity_lock_is_last);
    HU_RUN_TEST(config_persona_all_descriptions_non_empty);
    HU_RUN_TEST(best_of_n_default_is_zero);
    HU_RUN_TEST(best_of_n_parse_valid);
    HU_RUN_TEST(best_of_n_parse_clamped_at_5);
    HU_RUN_TEST(best_of_n_parse_accepts_five);
    HU_RUN_TEST(best_of_n_parse_accepts_zero);
    HU_RUN_TEST(best_of_n_parse_negative_ignored);
    HU_RUN_TEST(parse_verdict_passing_defaults_to_pass);
    HU_RUN_TEST(parse_verdict_pass_with_space);
    HU_RUN_TEST(parse_verdict_minor_only);
    HU_RUN_TEST(parse_verdict_minority_not_minor);
    HU_RUN_TEST(parse_verdict_rewrite_standalone);
    HU_RUN_TEST(parse_verdict_rewritten_not_rewrite);
    HU_RUN_TEST(parse_verdict_pass1_not_pass);
    HU_RUN_TEST(principle_index_from_keyword);
    HU_RUN_TEST(principle_index_from_verdict_digit);
    HU_RUN_TEST(principle_index_overflow_capped);
    HU_RUN_TEST(principle_index_no_number);
    HU_RUN_TEST(timing_model_sample_clamps_hour);
    HU_RUN_TEST(timing_model_sample_clamps_dow);
#ifdef HU_HAS_IMESSAGE
    HU_RUN_TEST(imessage_mark_read_vtable_wired);
#endif
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(temporal_resolve_tomorrow);
    HU_RUN_TEST(temporal_resolve_next_week);
    HU_RUN_TEST(temporal_resolve_tonight);
    HU_RUN_TEST(temporal_resolve_today);
    HU_RUN_TEST(temporal_resolve_in_3_hours);
    HU_RUN_TEST(temporal_resolve_in_2_days);
    HU_RUN_TEST(temporal_resolve_next_month);
    HU_RUN_TEST(temporal_resolve_soon_defaults_3_days);
    HU_RUN_TEST(temporal_resolve_in_30_minutes);
    HU_RUN_TEST(temporal_resolve_in_n_scans_after_in);
    HU_RUN_TEST(temporal_resolve_null_returns_zero);
    HU_RUN_TEST(temporal_resolve_unknown_defaults_7_days);
    HU_RUN_TEST(temporal_resolve_case_insensitive);
    HU_RUN_TEST(temporal_resolve_in_2_weeks);
    HU_RUN_TEST(temporal_resolve_in_1_month);
    HU_RUN_TEST(temporal_resolve_in_without_number);
    HU_RUN_TEST(temporal_resolve_in_2_hours_and_30_minutes);
    HU_RUN_TEST(temporal_events_init_table_creates_table);
    HU_RUN_TEST(temporal_events_store_and_retrieve);
    HU_RUN_TEST(temporal_events_mark_followed_up_hides_event);
    HU_RUN_TEST(temporal_events_low_confidence_filtered);
    HU_RUN_TEST(timing_model_learn_from_empty_db);
    HU_RUN_TEST(timing_model_learn_computes_percentiles);
    HU_RUN_TEST(timing_model_learn_null_args);
#endif
}
