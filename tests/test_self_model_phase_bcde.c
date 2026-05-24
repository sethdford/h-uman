/* tests/test_self_model_phase_bcde.c
 *
 * Spec 2026-05-19 self-model-scaffold — Phases B / C / D / E.
 *
 * Pins:
 *   AC-SM-1 (wire): hu_agent_m3_on_provider_success advances the behavior log
 *   AC-SM-2 (single write site): grep-based invariant
 *   AC-SM-3 (aggregation): tick inserts agent_self_observations row
 *   AC-SM-4 (world-model merge): merge populates recent_self_observations
 *   AC-SM-5 (drift): concern row inserted iff |sigma| >= threshold AND
 *                    baseline_n >= min_baseline_n
 *   AC-SM-7 (privacy): grep that no content-carrying field names appear in
 *                      src/agent/self_model.c or include/human/agent/self_model.h
 *
 * Uses the internal-#ifdef-wrap-with-stub-runner pattern. Runner symbol
 * resolves under both HU_ENABLE_SELF_MODEL=ON and =OFF.
 */

#include "human/agent/self_model.h"
#include "human/agent/world_model.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SELF_MODEL

/* ===================================================================
 * Phase B — wire-in tests + privacy + single-write-site invariants
 * =================================================================== */

#include "human/agent.h"

static void test_self_model_phase_b_on_provider_success_advances_log(void) {
    /* The full agent struct is heavyweight; we test the canonical write
     * helper directly via on_provider_success after zero-initializing
     * the agent and wiring just the behavior_log. */
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&agent.behavior_log, &alloc, 32), HU_OK);

    HU_ASSERT_EQ((long long)hu_agent_behavior_log_total_records(&agent.behavior_log), 0);

    /* Three fixture turns. on_provider_success is the canonical write
     * site; it should advance head by exactly one per call. */
    hu_agent_m3_on_provider_success(&agent);
    hu_agent_m3_on_provider_success(&agent);
    hu_agent_m3_on_provider_success(&agent);

    HU_ASSERT_EQ((long long)hu_agent_behavior_log_total_records(&agent.behavior_log), 3);

    hu_agent_behavior_log_destroy(&agent.behavior_log);
}

static void test_self_model_phase_b_stash_populates_record(void) {
    /* Stash a payload, fire on_provider_success, snapshot, assert the
     * stashed metric fields round-tripped. */
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&agent.behavior_log, &alloc, 16), HU_OK);

    hu_agent_behavior_stash_t s = {0};
    s.response_length_chars = 256;
    s.response_length_tokens_est = 64;
    s.tool_sequence_hash = 0xABCD1234u;
    s.tool_count = 3;
    s.emotional_register = (uint8_t)HU_AGENT_EMOTION_POSITIVE;
    s.persona_delta_kind = (uint8_t)HU_AGENT_PERSONA_DELTA_FORMALITY;
    s.response_latency_ms = 1234;
    hu_agent_m3_stash_behavior_metrics(&agent, &s);

    hu_agent_m3_on_provider_success(&agent);

    hu_agent_behavior_record_t out[2];
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&agent.behavior_log, out, 2, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 1);
    HU_ASSERT_EQ((long long)out[0].response_length_chars, 256);
    HU_ASSERT_EQ((long long)out[0].response_length_tokens_est, 64);
    HU_ASSERT_EQ((long long)out[0].tool_sequence_hash, (long long)0xABCD1234u);
    HU_ASSERT_EQ((long long)out[0].tool_count, 3);
    HU_ASSERT_EQ((long long)out[0].emotional_register, (long long)HU_AGENT_EMOTION_POSITIVE);
    HU_ASSERT_EQ((long long)out[0].persona_delta_kind, (long long)HU_AGENT_PERSONA_DELTA_FORMALITY);
    HU_ASSERT_EQ((long long)out[0].response_latency_ms, 1234);

    /* Stash must clear after emit — second turn with no re-stash sees
     * zeros for the metric fields. */
    hu_agent_m3_on_provider_success(&agent);
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&agent.behavior_log, out, 2, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 2);
    HU_ASSERT_EQ((long long)out[1].response_length_chars, 0);
    HU_ASSERT_EQ((long long)out[1].tool_sequence_hash, 0);

    hu_agent_behavior_log_destroy(&agent.behavior_log);
}

static void test_self_model_phase_b_contact_and_channel_hash_recorded(void) {
    /* The contact_hash and channel_id are derived from agent state
     * (memory_session_id, active_channel). Distinct contacts/channels
     * should produce distinct hash buckets. */
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&agent.behavior_log, &alloc, 16), HU_OK);

    const char *contact_a = "contact-a";
    agent.memory_session_id = contact_a;
    agent.memory_session_id_len = strlen(contact_a);
    const char *chan_a = "telegram";
    agent.active_channel = chan_a;
    agent.active_channel_len = strlen(chan_a);
    hu_agent_m3_on_provider_success(&agent);

    const char *contact_b = "contact-b";
    agent.memory_session_id = contact_b;
    agent.memory_session_id_len = strlen(contact_b);
    const char *chan_b = "imessage";
    agent.active_channel = chan_b;
    agent.active_channel_len = strlen(chan_b);
    hu_agent_m3_on_provider_success(&agent);

    hu_agent_behavior_record_t out[2];
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&agent.behavior_log, out, 2, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 2);
    /* Distinct inputs → distinct hashes (FNV-1a). Hashes are non-zero
     * for non-empty inputs. */
    HU_ASSERT(out[0].contact_hash != 0);
    HU_ASSERT(out[1].contact_hash != 0);
    HU_ASSERT(out[0].contact_hash != out[1].contact_hash);
    HU_ASSERT(out[0].channel_id != 0);
    HU_ASSERT(out[1].channel_id != 0);
    HU_ASSERT(out[0].channel_id != out[1].channel_id);

    /* Timestamps must be populated. */
    HU_ASSERT_GT((long long)out[0].timestamp_utc_ms, 0);
    HU_ASSERT_GT((long long)out[1].timestamp_utc_ms, 0);

    hu_agent_behavior_log_destroy(&agent.behavior_log);
}

static void test_self_model_phase_b_null_agent_safe(void) {
    /* on_provider_success / stash must tolerate NULL agent without
     * crashing — these are on the hot path. */
    hu_agent_m3_on_provider_success(NULL);
    hu_agent_m3_stash_behavior_metrics(NULL, NULL);
}

/* AC-SM-2 single write site: grep src/ for hu_agent_behavior_log_record(.
 * Exactly one occurrence is allowed (the canonical write site inside
 * hu_agent_internal_emit_behavior_record in src/agent/agent.c). The
 * definition lives in src/agent/self_model.c; that file is allowed to
 * have the prototype/definition. Comments referencing the symbol in
 * non-call form (e.g. "hu_agent_behavior_log_record appears once") are
 * filtered by requiring an opening parenthesis. */
static void test_self_model_phase_b_single_write_site_invariant(void) {
    /* Count call-site lines of `hu_agent_behavior_log_record(` outside
     * src/agent/self_model.c (which holds the definition). Lines whose
     * first non-space char is `*` (block-comment continuation) or `//`
     * are filtered so descriptive references in code comments do not
     * inflate the count. */
    const char *script = "set -u\n"
                         "src_root=\"$(cd \"$(dirname \"$0\")\" 2>/dev/null && pwd || pwd)\"\n"
                         "if [ -d \"$src_root/src\" ]; then root=\"$src_root\"; "
                         "elif [ -d \"$src_root/../src\" ]; then root=\"$src_root/..\"; "
                         "else root=\".\"; fi\n"
                         "count=$(grep -rn 'hu_agent_behavior_log_record(' \"$root/src\" | "
                         "grep -v 'self_model.c' | "
                         "grep -v -E ':[[:space:]]*\\*' | "
                         "grep -v -E ':[[:space:]]*//' | "
                         "wc -l | tr -d ' ')\n"
                         "echo $count\n";
    FILE *p = popen(script, "r");
    HU_ASSERT_NOT_NULL(p);
    int count = -1;
    if (fscanf(p, "%d", &count) != 1)
        count = -2;
    pclose(p);
    /* Exactly one call-site outside src/agent/self_model.c (the
     * definition site). Two would mean a duplicate; zero would mean
     * the wire-in was removed. */
    HU_ASSERT_EQ(count, 1);
}

/* AC-SM-7 privacy hygiene: assert disallowed content-field tokens do
 * NOT appear in src/agent/self_model.c or include/human/agent/self_model.h.
 *
 * Tokens checked (with allowlist for technical suffixes that are
 * benign): body, content, message, prompt, response, text, args. The
 * allowlist permits "response_length_*", "response_latency_*", which
 * are scalar (size/timing), not content. The check uses awk so we can
 * exclude comments — false positives in cautionary comments would
 * defeat the rule. */
static void test_self_model_phase_b_no_content_capture(void) {
    const char *script =
        "set -u\n"
        "src_root=\"$(cd \"$(dirname \"$0\")\" 2>/dev/null && pwd || pwd)\"\n"
        "if [ -d \"$src_root/src\" ]; then root=\"$src_root\"; "
        "elif [ -d \"$src_root/../src\" ]; then root=\"$src_root/..\"; "
        "else root=\".\"; fi\n"
        "files=\"$root/src/agent/self_model.c $root/include/human/agent/self_model.h\"\n"
        /* Strip line- and block-comments before checking for disallowed
         * tokens. The grep then looks for the bare token as an identifier
         * fragment that would indicate a content-capturing field. */
        "for f in $files; do\n"
        "  awk '\n"
        "    BEGIN { in_block = 0 }\n"
        "    {\n"
        "      line = $0\n"
        "      out = \"\"\n"
        "      while (length(line) > 0) {\n"
        "        if (in_block) {\n"
        "          i = index(line, \"*/\")\n"
        "          if (i == 0) { line = \"\"; break }\n"
        "          line = substr(line, i + 2); in_block = 0\n"
        "        } else {\n"
        "          a = index(line, \"/*\"); b = index(line, \"//\")\n"
        "          if (a == 0 && b == 0) { out = out line; break }\n"
        "          if (a > 0 && (b == 0 || a < b)) {\n"
        "            out = out substr(line, 1, a - 1)\n"
        "            line = substr(line, a + 2); in_block = 1\n"
        "          } else {\n"
        "            out = out substr(line, 1, b - 1); break\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "      print out\n"
        "    }\n"
        "  ' \"$f\"\n"
        "done | "
        /* Allowlist scalar suffixes by deleting them before the grep. */
        "sed -e 's/response_length_chars//g' -e 's/response_length_tokens_est//g' "
        "-e 's/response_length_mean//g' -e 's/response_length_stddev//g' "
        "-e 's/response_length_pending//g' "
        "-e 's/response_latency_ms//g' | "
        "grep -E -c '\\<(body|content|message|prompt|response|text|args)\\>'\n";
    FILE *p = popen(script, "r");
    HU_ASSERT_NOT_NULL(p);
    int count = -1;
    if (fscanf(p, "%d", &count) != 1)
        count = -2;
    pclose(p);
    /* Zero matches is the only passing state. Anything else means a
     * content-capturing identifier slipped into either file. */
    HU_ASSERT_EQ(count, 0);
}

/* ===================================================================
 * Phase C — schema migration, aggregation tick, drift signal
 * =================================================================== */

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

static sqlite3 *self_model_phase_c_open_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_agent_self_model_init_tables(db), HU_OK);
    return db;
}

static hu_agent_behavior_record_t self_model_phase_c_make_record(uint32_t len, uint32_t latency_ms,
                                                                 uint32_t tool_seq, uint8_t emotion,
                                                                 int64_t ts_ms) {
    hu_agent_behavior_record_t r;
    memset(&r, 0, sizeof(r));
    r.response_length_chars = len;
    r.response_length_tokens_est = len / 4;
    r.tool_sequence_hash = tool_seq;
    r.tool_count = (uint16_t)(tool_seq & 0xF);
    r.emotional_register = emotion;
    r.response_latency_ms = latency_ms;
    r.timestamp_utc_ms = ts_ms;
    return r;
}

static int64_t self_model_phase_c_count_rows(sqlite3 *db, const char *table) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        if (st)
            sqlite3_finalize(st);
        return -1;
    }
    int64_t out = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

static void test_self_model_phase_c_init_tables_creates_schema(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_agent_self_model_init_tables(db), HU_OK);
    /* Idempotency: second call must not fail. */
    HU_ASSERT_EQ(hu_agent_self_model_init_tables(db), HU_OK);
    /* Both tables present and empty. */
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_observations"), 0);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_concerns"), 0);
    sqlite3_close(db);
}

static void test_self_model_phase_c_compute_and_insert_observation(void) {
    sqlite3 *db = self_model_phase_c_open_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 64), HU_OK);

    /* 10 records, varying lengths and latencies, two tool sequences. */
    for (int i = 0; i < 10; i++) {
        hu_agent_behavior_record_t r = self_model_phase_c_make_record(
            (uint32_t)(100 + i * 10), (uint32_t)(50 + i * 5), (uint32_t)(i % 2), (uint8_t)(i % 5),
            (int64_t)(1000 + i));
        HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    }

    int64_t obs_id = 0;
    hu_agent_self_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    HU_ASSERT_EQ(hu_agent_self_model_compute_and_insert_observation(db, &log, /*window_n*/ 100,
                                                                    /*now_ts_ms*/ 5000, &obs_id,
                                                                    &obs),
                 HU_OK);
    HU_ASSERT_GT((long long)obs_id, 0);
    HU_ASSERT_EQ((long long)obs.n_turns, 10);
    /* Mean of 100..190 step 10 = 145. */
    HU_ASSERT(obs.response_length_mean > 144.0 && obs.response_length_mean < 146.0);
    HU_ASSERT_GT(obs.response_length_stddev, 0.0);
    /* Two equally-frequent tool sequences -> entropy is 1 bit. */
    HU_ASSERT(obs.tool_selection_entropy > 0.9 && obs.tool_selection_entropy < 1.1);
    /* p50 of sorted [50,55,60,65,70,75,80,85,90,95] via nearest-rank
     * (idx = pct * (n-1) = 0.5 * 9 = 4.5, truncated -> index 4 -> 70).
     * p95 idx = 0.95 * 9 = 8.55 -> 8 -> 90. Nearest-rank, no interpolation. */
    HU_ASSERT_EQ((long long)obs.latency_p50_ms, 70);
    HU_ASSERT_EQ((long long)obs.latency_p95_ms, 90);

    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_observations"), 1);

    hu_agent_behavior_log_destroy(&log);
    sqlite3_close(db);
}

static void test_self_model_phase_c_compute_empty_log_is_noop(void) {
    sqlite3 *db = self_model_phase_c_open_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 16), HU_OK);

    int64_t obs_id = 999;
    HU_ASSERT_EQ(
        hu_agent_self_model_compute_and_insert_observation(db, &log, 100, 5000, &obs_id, NULL),
        HU_OK);
    HU_ASSERT_EQ((long long)obs_id, 0);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_observations"), 0);

    hu_agent_behavior_log_destroy(&log);
    sqlite3_close(db);
}

static void test_self_model_phase_c_aggregate_tick_inserts_observation(void) {
#if HU_IS_TEST
    hu_daemon_tick_self_observation_aggregate_reset_warn_guards_for_test();
#endif
    sqlite3 *db = self_model_phase_c_open_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 64), HU_OK);
    for (int i = 0; i < 5; i++) {
        hu_agent_behavior_record_t r = self_model_phase_c_make_record(
            (uint32_t)(100 + i), (uint32_t)50, 0, 0, (int64_t)(1000 + i));
        HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    }

    int64_t last_ts = 0;
    size_t last_total = 0;
    HU_ASSERT_EQ(hu_daemon_tick_self_observation_aggregate(
                     db, &log, /*now_ts*/ 5000, &last_ts, &last_total,
                     /*every_n_turns*/ 10, /*every_sec*/ 60,
                     /*baseline_mean*/ 0.0, /*baseline_stddev*/ 0.0, /*baseline_n*/ 0,
                     /*drift_threshold*/ 2.0, /*min_baseline_n*/ 50),
                 HU_OK);
    /* First tick with non-empty log fires the first_tick branch. */
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_observations"), 1);
    HU_ASSERT_EQ((long long)last_ts, 5000);
    HU_ASSERT_EQ((long long)last_total, 5);

    hu_agent_behavior_log_destroy(&log);
    sqlite3_close(db);
}

static void test_self_model_phase_c_aggregate_tick_respects_interval(void) {
#if HU_IS_TEST
    hu_daemon_tick_self_observation_aggregate_reset_warn_guards_for_test();
#endif
    sqlite3 *db = self_model_phase_c_open_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 64), HU_OK);
    for (int i = 0; i < 3; i++) {
        hu_agent_behavior_record_t r =
            self_model_phase_c_make_record(100, 50, 0, 0, (int64_t)(1000 + i));
        HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    }
    int64_t last_ts = 0;
    size_t last_total = 0;
    /* First call fires (first_tick branch). */
    HU_ASSERT_EQ(hu_daemon_tick_self_observation_aggregate(db, &log, 5000, &last_ts, &last_total,
                                                           100, 60, 0.0, 0.0, 0, 2.0, 50),
                 HU_OK);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_observations"), 1);

    /* Second call with only +1 turn elapsed and only +1 second elapsed
     * should NOT fire (every_n_turns=100, every_sec=60). */
    hu_agent_behavior_record_t r = self_model_phase_c_make_record(100, 50, 0, 0, (int64_t)1010);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    HU_ASSERT_EQ(hu_daemon_tick_self_observation_aggregate(db, &log, 5500, &last_ts, &last_total,
                                                           100, 60, 0.0, 0.0, 0, 2.0, 50),
                 HU_OK);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_observations"), 1);

    hu_agent_behavior_log_destroy(&log);
    sqlite3_close(db);
}

static void test_self_model_phase_c_aggregate_tick_disabled_when_zero_config(void) {
#if HU_IS_TEST
    hu_daemon_tick_self_observation_aggregate_reset_warn_guards_for_test();
#endif
    sqlite3 *db = self_model_phase_c_open_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 16), HU_OK);
    hu_agent_behavior_record_t r = self_model_phase_c_make_record(100, 50, 0, 0, (int64_t)1000);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    int64_t last_ts = 0;
    size_t last_total = 0;
    /* Both watermarks zero -> disabled subsystem -> log message + no-op. */
    HU_ASSERT_EQ(hu_daemon_tick_self_observation_aggregate(db, &log, 5000, &last_ts, &last_total,
                                                           /*every_n_turns*/ 0, /*every_sec*/ 0,
                                                           0.0, 0.0, 0, 2.0, 50),
                 HU_OK);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_observations"), 0);
    HU_ASSERT_EQ((long long)last_ts, 0);

    hu_agent_behavior_log_destroy(&log);
    sqlite3_close(db);
}

static void test_self_model_phase_c_drift_signal_fires_above_threshold(void) {
    sqlite3 *db = self_model_phase_c_open_db();
    /* Synthetic observation with mean=500, baseline mean=100, stddev=50.
     * Sigma = (500 - 100)/50 = 8.0 — well above 2.0 threshold. */
    hu_agent_self_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    obs.n_turns = 100;
    obs.response_length_mean = 500.0;
    obs.response_length_stddev = 5.0;

    /* Pre-insert a row so observation_id has something to FK against
     * (the column is FOREIGN KEY but SQLite doesn't enforce by default;
     * we still want a real id). */
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "INSERT INTO agent_self_observations(window_start_ts_ms,"
                                    "window_end_ts_ms,n_turns,response_length_mean,"
                                    "response_length_stddev,tool_selection_entropy,"
                                    "latency_p50_ms,latency_p95_ms) "
                                    "VALUES(0,0,100,500.0,5.0,0.0,0,0)",
                                    -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    int64_t obs_id = sqlite3_last_insert_rowid(db);

    uint32_t concerns = 0;
    HU_ASSERT_EQ(hu_agent_self_model_detect_drift(db, &obs, obs_id, /*baseline_mean*/ 100.0,
                                                  /*baseline_stddev*/ 50.0, /*baseline_n*/ 200,
                                                  /*drift_threshold*/ 2.0, /*min_baseline_n*/ 50,
                                                  /*now_ts*/ 6000, &concerns),
                 HU_OK);
    HU_ASSERT_EQ((long long)concerns, 1);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_concerns"), 1);
    sqlite3_close(db);
}

static void test_self_model_phase_c_drift_signal_requires_minimum_baseline(void) {
    sqlite3 *db = self_model_phase_c_open_db();
    hu_agent_self_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    obs.n_turns = 100;
    obs.response_length_mean = 500.0;

    uint32_t concerns = 0;
    /* baseline_n=10 < min_baseline_n=50 -> early return with zero
     * concerns inserted even though the sigma is huge. */
    HU_ASSERT_EQ(hu_agent_self_model_detect_drift(db, &obs, /*obs_id*/ 1, 100.0, 50.0,
                                                  /*baseline_n*/ 10,
                                                  /*drift_threshold*/ 2.0, /*min_baseline_n*/ 50,
                                                  6000, &concerns),
                 HU_OK);
    HU_ASSERT_EQ((long long)concerns, 0);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_concerns"), 0);
    sqlite3_close(db);
}

static void test_self_model_phase_c_drift_signal_no_fire_within_threshold(void) {
    sqlite3 *db = self_model_phase_c_open_db();
    hu_agent_self_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    obs.n_turns = 100;
    obs.response_length_mean = 110.0; /* small deviation */

    uint32_t concerns = 0;
    /* sigma = (110 - 100)/50 = 0.2 — well under 2.0. */
    HU_ASSERT_EQ(hu_agent_self_model_detect_drift(db, &obs, /*obs_id*/ 1, 100.0, 50.0,
                                                  /*baseline_n*/ 200,
                                                  /*drift_threshold*/ 2.0, /*min_baseline_n*/ 50,
                                                  6000, &concerns),
                 HU_OK);
    HU_ASSERT_EQ((long long)concerns, 0);
    HU_ASSERT_EQ((long long)self_model_phase_c_count_rows(db, "agent_self_concerns"), 0);
    sqlite3_close(db);
}

/* ===================================================================
 * Phase D — world-model merge
 * =================================================================== */

static void test_self_model_phase_d_world_model_merge_populates_observations(void) {
    sqlite3 *db = self_model_phase_c_open_db();
    /* Insert 3 synthetic rows. */
    for (int i = 0; i < 3; i++) {
        sqlite3_stmt *st = NULL;
        HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                        "INSERT INTO agent_self_observations("
                                        "window_start_ts_ms,window_end_ts_ms,n_turns,"
                                        "response_length_mean,response_length_stddev,"
                                        "tool_selection_entropy,latency_p50_ms,latency_p95_ms) "
                                        "VALUES(?,?,?,?,?,?,?,?)",
                                        -1, &st, NULL),
                     SQLITE_OK);
        sqlite3_bind_int64(st, 1, (int64_t)(1000 + i * 100));
        sqlite3_bind_int64(st, 2, (int64_t)(2000 + i * 100));
        sqlite3_bind_int64(st, 3, (int64_t)(10 + i));
        sqlite3_bind_double(st, 4, 100.0 + i);
        sqlite3_bind_double(st, 5, 5.0);
        sqlite3_bind_double(st, 6, 1.5);
        sqlite3_bind_int64(st, 7, (int64_t)(50 + i));
        sqlite3_bind_int64(st, 8, (int64_t)(90 + i));
        HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
        sqlite3_finalize(st);
    }

    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    hu_world_model_merge_self_observations(&wm, db);
    HU_ASSERT_EQ((long long)wm.self_model.recent_self_observations_count, 3);
    /* Newest-first ordering: row inserted last (i=2) should appear at slot 0. */
    HU_ASSERT_EQ((long long)wm.self_model.recent_self_observations[0].n_turns, 12);
    HU_ASSERT_EQ((long long)wm.self_model.recent_self_observations[1].n_turns, 11);
    HU_ASSERT_EQ((long long)wm.self_model.recent_self_observations[2].n_turns, 10);
    HU_ASSERT(wm.self_model.recent_self_observations[0].response_length_mean > 101.5);
    HU_ASSERT(wm.self_model.recent_self_observations[0].response_length_mean < 102.5);

    sqlite3_close(db);
}

static void test_self_model_phase_d_world_model_merge_handles_empty_table(void) {
    sqlite3 *db = self_model_phase_c_open_db();
    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    /* Pre-populate the array with garbage so we can confirm merge zeroes
     * it cleanly. */
    wm.self_model.recent_self_observations_count = 7;
    wm.self_model.recent_self_observations[0].n_turns = 999;

    hu_world_model_merge_self_observations(&wm, db);
    HU_ASSERT_EQ((long long)wm.self_model.recent_self_observations_count, 0);
    HU_ASSERT_EQ((long long)wm.self_model.recent_self_observations[0].n_turns, 0);

    sqlite3_close(db);
}

static void test_self_model_phase_d_merge_null_db_is_safe(void) {
    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    wm.self_model.recent_self_observations_count = 3;
    hu_world_model_merge_self_observations(&wm, NULL);
    /* NULL db means "no signal" — count cleared, no crash. */
    HU_ASSERT_EQ((long long)wm.self_model.recent_self_observations_count, 0);
}

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_ENABLE_SELF_MODEL */

void run_self_model_phase_bcde_tests(void);
void run_self_model_phase_bcde_tests(void) {
    HU_TEST_SUITE("self_model_phase_bcde");
#ifdef HU_ENABLE_SELF_MODEL
    HU_RUN_TEST(test_self_model_phase_b_on_provider_success_advances_log);
    HU_RUN_TEST(test_self_model_phase_b_stash_populates_record);
    HU_RUN_TEST(test_self_model_phase_b_contact_and_channel_hash_recorded);
    HU_RUN_TEST(test_self_model_phase_b_null_agent_safe);
    HU_RUN_TEST(test_self_model_phase_b_single_write_site_invariant);
    HU_RUN_TEST(test_self_model_phase_b_no_content_capture);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_self_model_phase_c_init_tables_creates_schema);
    HU_RUN_TEST(test_self_model_phase_c_compute_and_insert_observation);
    HU_RUN_TEST(test_self_model_phase_c_compute_empty_log_is_noop);
    HU_RUN_TEST(test_self_model_phase_c_aggregate_tick_inserts_observation);
    HU_RUN_TEST(test_self_model_phase_c_aggregate_tick_respects_interval);
    HU_RUN_TEST(test_self_model_phase_c_aggregate_tick_disabled_when_zero_config);
    HU_RUN_TEST(test_self_model_phase_c_drift_signal_fires_above_threshold);
    HU_RUN_TEST(test_self_model_phase_c_drift_signal_requires_minimum_baseline);
    HU_RUN_TEST(test_self_model_phase_c_drift_signal_no_fire_within_threshold);
    HU_RUN_TEST(test_self_model_phase_d_world_model_merge_populates_observations);
    HU_RUN_TEST(test_self_model_phase_d_world_model_merge_handles_empty_table);
    HU_RUN_TEST(test_self_model_phase_d_merge_null_db_is_safe);
#endif /* HU_ENABLE_SQLITE */
#endif /* HU_ENABLE_SELF_MODEL */
}
