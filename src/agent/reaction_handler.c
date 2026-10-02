/* src/agent/reaction_handler.c
 *
 * Phase 2 Task 13 (RL SOTA): hu_reaction_event_t → hu_preference_pair_t
 * row in the daemon-owned hu_dpo_collector_t. See the header for the
 * full wiring diagram.
 *
 * Phase 5 R4: the lookup store. Two implementations live in this TU
 * behind a feature flag:
 *
 *   HU_IS_TEST              → in-memory array (deterministic, no disk I/O)
 *   HU_ENABLE_SQLITE        → SQLite-backed persistent store at
 *                             ~/.human/reaction_lookup.db (production)
 *
 * The SQLite path replaces the previous 256-entry in-memory ring, which
 * silently dropped registrations once full AND lost ALL state on daemon
 * restart (R4 in the Phase-5 risk register). */
#include "human/agent/reaction_handler.h"
#include "human/channels/imessage_ingest.h"
#include "human/contact_send_recency.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/core/paths.h"
#include "human/memory/identity_resolver.h"
#include "human/memory/outbound_sends_repo.h"
#include "human/memory/personal_model.h"
#include "human/ml/dpo.h"
#include "human/reflection.h" /* T8: retire-on-contradiction */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#include <sys/stat.h>
#endif

#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
#define HU_RXN_LOOKUP_USES_SQLITE 1
#else
#define HU_RXN_LOOKUP_USES_SQLITE 0
#endif

/* ----- in-memory backing store (kept for the HU_IS_TEST path) -----
 *
 * Under HU_IS_TEST we keep the original array-based lookup so unit tests
 * remain deterministic and never touch real disk. The cap is intentionally
 * loose (1024) so larger test scenarios don't hit the previous 256-entry
 * silent-drop. */
#if HU_RXN_LOOKUP_USES_SQLITE == 0
#define LOOKUP_CAP 1024

typedef struct {
    char channel[32];
    char thread[128];
    char msg_ref[128];
    char prompt[2048];
    char response[4096];
    char alternative[4096];
    int64_t inserted_at; /* unix seconds, as the SQLite store's column */
} lookup_entry_t;

static lookup_entry_t s_lookup[LOOKUP_CAP];
static size_t s_lookup_n = 0;
#endif

/* Daemon-owned collector handle. NULL until set_collector is called. */
static hu_dpo_collector_t *s_collector = NULL;

/* Phase 1c of docs/plans/2026-05-18-imessage-sota.md: optional personal-model
 * sink. When non-NULL, iMessage reactions on registered assistant messages
 * are also ingested into the personal model (separate from the DPO collector
 * which exists for training-data collection). Mirrors the set_collector
 * pattern: the daemon sets it at init via
 * hu_reaction_handler_set_personal_model. */
static hu_personal_model_t *s_personal_model = NULL;
/* Sprint A.7: optional identity-graph wire. NULL == no canonicalization;
 * non-NULL == reactions are looked up via hu_identity_lookup before
 * ingest, and HIGH-confidence merges rewrite sender_handle to the
 * canonical name. */
static const hu_identity_graph_t *s_identity_graph = NULL;

/* T8 (reflection retire-on-contradiction): optional reflection-DB wire.
 * NULL == no retire-on-contradiction; non-NULL == a NEGATIVE reaction
 * retires patterns surfaced in that channel within the contradiction
 * window. Daemon owns the handle and sets it at init. */
static struct sqlite3 *s_reflection_db = NULL;

/* Per-turn signal flag (NOT thread-safe; daemon is single-threaded event loop —
 * see header comment on hu_reaction_handler_clear_turn for the full safety
 * argument. If the daemon ever gains concurrent turn dispatch, move this onto
 * hu_agent_t as a per-agent field). */
static int s_called_this_turn = 0;

void hu_reaction_handler_set_collector(hu_dpo_collector_t *c) {
    s_collector = c;
}

void hu_reaction_handler_set_personal_model(hu_personal_model_t *m) {
    s_personal_model = m;
}

void hu_reaction_handler_set_identity_graph(const hu_identity_graph_t *graph) {
    s_identity_graph = graph;
}
void hu_reaction_handler_set_reflection_db(struct sqlite3 *db) {
    s_reflection_db = db;
}
void hu_reaction_handler_clear_turn(void) {
    s_called_this_turn = 0;
}
int hu_reaction_handler_was_called_this_turn(void) {
    return s_called_this_turn;
}

/* ===== SQLite-backed persistent lookup store =====
 *
 * Schema (created lazily on first open):
 *   reaction_lookup(channel TEXT, thread TEXT, msg_ref TEXT,
 *                   prompt TEXT, response TEXT, inserted_at INTEGER,
 *                   PRIMARY KEY (channel, thread, msg_ref))
 *
 * Writes go through INSERT OR REPLACE for upsert semantics — if a
 * (channel, thread, msg_ref) triple is re-registered, the latest
 * prompt/response wins. SQLite's WAL journal mode gives us atomic
 * commits without needing the tmp+fsync+rename dance used elsewhere
 * (see src/memory/personal_model.c for the file-based equivalent).
 *
 * Retention: every register call probes a counter and runs a 60-day
 * cleanup DELETE every 1000th invocation. Keeps the DB bounded without
 * a daemon-side scheduler tick. */
#if defined(HU_ENABLE_SQLITE)

/* Ensure parent directory exists for ~/.human/reaction_lookup.db. mkdir(0700)
 * matches the rest of the ~/.human/ tree posture. Best-effort; failures get
 * surfaced when sqlite3_open is unable to create the DB file. */
static void rxn_ensure_parent_dir(const char *path) {
    if (!path)
        return;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash)
        return;
    *slash = '\0';
    (void)mkdir(buf, 0700);
}

/* Open + migrate the lookup store at `path`. Shared by the production open
 * (fixed ~/.human path, cached handle) and the HU_IS_TEST seam (arbitrary
 * path) so the migration contract is unit-testable. Returns 1 with *out_db
 * set on success, 0 with *out_db NULL on failure. */
static int rxn_db_open_at(const char *path, sqlite3 **out_db) {
    *out_db = NULL;
    rxn_ensure_parent_dir(path);

    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return 0;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    static const char *ddl[] = {
        "CREATE TABLE IF NOT EXISTS reaction_lookup ("
        "channel TEXT NOT NULL,"
        "thread TEXT NOT NULL,"
        "msg_ref TEXT NOT NULL,"
        "prompt TEXT NOT NULL,"
        "response TEXT NOT NULL,"
        "alternative TEXT,"
        "inserted_at INTEGER NOT NULL,"
        "PRIMARY KEY (channel, thread, msg_ref))",
        "CREATE INDEX IF NOT EXISTS idx_reaction_lookup_inserted "
        "ON reaction_lookup(inserted_at DESC)",
        NULL,
    };
    for (size_t i = 0; ddl[i]; i++) {
        if (sqlite3_exec(db, ddl[i], NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            return 0;
        }
    }

    /* Legacy migration, best-effort: the duplicate-column error on a store
     * that already has `alternative` (including every fresh create above) is
     * the benign already-migrated case — same contract as graph.c MIGRATION.
     * Treating it as fatal bricked every open after the column first landed,
     * silently killing registration AND lookup (zero imessage_tapback DPO
     * pairs, 2026-05-31 → 2026-07-19). */
    sqlite3_exec(db, "ALTER TABLE reaction_lookup ADD COLUMN alternative TEXT", NULL, NULL, NULL);

    *out_db = db;
    return 1;
}

#endif /* HU_ENABLE_SQLITE */

#if HU_RXN_LOOKUP_USES_SQLITE

static sqlite3 *s_db = NULL;
static unsigned long s_register_count = 0;
static const unsigned long RXN_CLEANUP_EVERY_N = 1000;
static const long RXN_RETENTION_SECONDS = 60L * 86400L; /* 60 days */

static int rxn_db_open(void) {
    if (s_db)
        return 1;

    static char path_buf[1024];
    (void)hu_paths_state_or(path_buf, sizeof(path_buf), "/tmp", "reaction_lookup.db");
    return rxn_db_open_at(path_buf, &s_db);
}

/* 60-day retention sweep. Idempotent; safe to call repeatedly. */
static void rxn_db_cleanup_old(void) {
    if (!s_db)
        return;
    sqlite3_stmt *st = NULL;
    static const char sql[] =
        "DELETE FROM reaction_lookup WHERE inserted_at < (strftime('%s','now') - ?)";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)RXN_RETENTION_SECONDS);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static void rxn_db_register(const char *channel, const char *thread, const char *msg_ref,
                            const char *prompt, const char *response, const char *alternative) {
    if (!rxn_db_open())
        return;

    sqlite3_stmt *st = NULL;
    static const char sql[] =
        "INSERT OR REPLACE INTO reaction_lookup "
        "(channel, thread, msg_ref, prompt, response, alternative, inserted_at) "
        "VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'))";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, thread, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, msg_ref, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, prompt, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, response, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 6, alternative ? alternative : "", -1, SQLITE_STATIC);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);

    /* Periodic retention sweep. Bounded by RXN_CLEANUP_EVERY_N to keep
     * the per-register hot path cheap. */
    if (++s_register_count % RXN_CLEANUP_EVERY_N == 0)
        rxn_db_cleanup_old();
}

/* Step a prepared (prompt, response, alternative) SELECT once, copy the row
 * into the caller's buffers, finalize. Returns 1 on a row, 0 otherwise. */
static int rxn_db_take_row(sqlite3_stmt *st, char *prompt_out, size_t prompt_cap,
                           char *response_out, size_t response_cap, char *alternative_out,
                           size_t alternative_cap) {
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(st, 0);
        const unsigned char *r = sqlite3_column_text(st, 1);
        const unsigned char *a = sqlite3_column_text(st, 2);
        snprintf(prompt_out, prompt_cap, "%s", p ? (const char *)p : "");
        snprintf(response_out, response_cap, "%s", r ? (const char *)r : "");
        if (alternative_out && alternative_cap > 0)
            snprintf(alternative_out, alternative_cap, "%s", a ? (const char *)a : "");
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* Lookup returns 1 on hit, 0 on miss. On hit, prompt_out/response_out/alternative_out are
 * filled (truncated via snprintf if needed). alternative_out and alternative_cap may be NULL
 * if the caller doesn't need the alternative. */
static int rxn_db_lookup(const char *channel, const char *thread, const char *msg_ref,
                         char *prompt_out, size_t prompt_cap, char *response_out,
                         size_t response_cap, char *alternative_out, size_t alternative_cap) {
    if (!rxn_db_open())
        return 0;

    sqlite3_stmt *st = NULL;
    static const char sql[] =
        "SELECT prompt, response, COALESCE(alternative, '') FROM reaction_lookup "
        "WHERE channel = ? AND thread = ? AND msg_ref = ? LIMIT 1";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, thread, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, msg_ref, -1, SQLITE_STATIC);

    return rxn_db_take_row(st, prompt_out, prompt_cap, response_out, response_cap, alternative_out,
                           alternative_cap);
}

#endif /* HU_RXN_LOOKUP_USES_SQLITE */

#ifdef HU_ENABLE_SQLITE
/* Doctor probe (PR #321 follow-up): report whether the production lookup
 * store is actually usable. Reuses rxn_db_open()'s cached handle — the
 * exact path registration and tapback lookup take — so a probe success
 * IS a recorder-path success, and no second connection lifecycle exists.
 * Under HU_IS_TEST the active backend is the in-memory ring, which
 * cannot fail to open; the probe reports healthy so doctor checks in
 * test binaries don't touch the real ~/.human tree. */
int hu_reaction_handler_lookup_db_probe(void) {
#if HU_RXN_LOOKUP_USES_SQLITE
    return rxn_db_open();
#else
    return 1;
#endif
}
#endif /* HU_ENABLE_SQLITE */

/* ===== Unified lookup adapter =====
 *
 * Returns 1 on hit (prompt_out / response_out / alternative_out filled), 0 on miss.
 * Buffers must be sized at least 2048 (prompt) and 4096 (response/alternative) to
 * match hu_preference_pair_t's fixed-size columns. alternative_out and alternative_cap
 * may be NULL if the caller doesn't need the alternative. */
static int reaction_lookup_find(const hu_reaction_event_t *e, char *prompt_out, size_t prompt_cap,
                                char *response_out, size_t response_cap, char *alternative_out,
                                size_t alternative_cap) {
    const char *thread = e->target_thread_id ? e->target_thread_id : "";
    const char *msg_ref = e->target_message_ref ? e->target_message_ref : "";

#if HU_RXN_LOOKUP_USES_SQLITE
    return rxn_db_lookup(e->channel_id, thread, msg_ref, prompt_out, prompt_cap, response_out,
                         response_cap, alternative_out, alternative_cap);
#else
    for (size_t i = 0; i < s_lookup_n; i++) {
        if (strcmp(s_lookup[i].channel, e->channel_id) == 0 &&
            strcmp(s_lookup[i].thread, thread) == 0 && strcmp(s_lookup[i].msg_ref, msg_ref) == 0) {
            snprintf(prompt_out, prompt_cap, "%s", s_lookup[i].prompt);
            snprintf(response_out, response_cap, "%s", s_lookup[i].response);
            if (alternative_out && alternative_cap > 0)
                snprintf(alternative_out, alternative_cap, "%s", s_lookup[i].alternative);
            return 1;
        }
    }
    return 0;
#endif
}

/* DEF-8: the reply a tapback is about, anchored on ONE delivery the daemon
 * made (outbound_sends): the registration in the same thread closest at or
 * before that delivery, no earlier than the FU-1 window before it — no
 * proactive or scheduled send reaches a contact within that window of a
 * reactive reply, so every daemon bubble inside it belongs to that reply.
 * Used when the exact msg_ref join misses: in production the router fell
 * back to a synthetic "out-<ts>" ref for 440 of 492 registrations because
 * chat.db `text` is NULL for ~98% of our sent rows. */
#define RXN_DELIVERY_SLACK_MS 5000 /* outbound_sends vs chat.db date: 192/197 within 3 s */
#define RXN_ANCHOR_SLACK_S    5    /* registration vs first delivery: 73/73 within 5 s */

static int reaction_lookup_find_anchored(const hu_reaction_event_t *e, int64_t anchor_s,
                                         char *prompt_out, size_t prompt_cap, char *response_out,
                                         size_t response_cap, char *alternative_out,
                                         size_t alternative_cap) {
    const char *thread = e->target_thread_id ? e->target_thread_id : "";
    int64_t lo = anchor_s - HU_DAEMON_REACTIVE_GATE_WINDOW_S;
    int64_t hi = anchor_s + RXN_ANCHOR_SLACK_S;
#if HU_RXN_LOOKUP_USES_SQLITE
    if (!rxn_db_open())
        return 0;
    sqlite3_stmt *st = NULL;
    static const char sql[] =
        "SELECT prompt, response, COALESCE(alternative, '') FROM reaction_lookup "
        "WHERE channel = ? AND thread = ? AND inserted_at BETWEEN ? AND ? "
        "ORDER BY inserted_at DESC LIMIT 1";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, e->channel_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, thread, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)lo);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)hi);
    return rxn_db_take_row(st, prompt_out, prompt_cap, response_out, response_cap, alternative_out,
                           alternative_cap);
#else
    const lookup_entry_t *best = NULL;
    for (size_t i = 0; i < s_lookup_n; i++) {
        const lookup_entry_t *x = &s_lookup[i];
        if (strcmp(x->channel, e->channel_id) != 0 || strcmp(x->thread, thread) != 0)
            continue;
        if (x->inserted_at < lo || x->inserted_at > hi)
            continue;
        if (!best || x->inserted_at >= best->inserted_at)
            best = x;
    }
    if (!best)
        return 0;
    snprintf(prompt_out, prompt_cap, "%s", best->prompt);
    snprintf(response_out, response_cap, "%s", best->response);
    snprintf(alternative_out, alternative_cap, "%s", best->alternative);
    return 1;
#endif
}

#if HU_IS_TEST
static int s_outcome_join_override = -1;
void hu_reaction_handler_set_outcome_join_mode_for_test(int mode) {
    s_outcome_join_override = mode;
}
#endif

static hu_gate_mode_t outcome_join_mode(void) {
#if HU_IS_TEST
    if (s_outcome_join_override >= 0)
        return (hu_gate_mode_t)s_outcome_join_override;
#endif
    return hu_gate_mode_from_env("HU_OUTCOME_JOIN", HU_GATE_OFF);
}

static hu_reaction_engagement_sink_fn s_engagement_sink = NULL;
void hu_reaction_handler_set_engagement_sink(hu_reaction_engagement_sink_fn fn) {
    s_engagement_sink = fn;
}

/* Did the daemon deliver the reacted-to message? Only a contact's reaction on
 * an is_from_me row that a recorded send claims (outbound_sends) counts —
 * Seth's own typing is is_from_me too. Returns the delivery time (ms) or 0. */
static int64_t outcome_join_delivery(const hu_reaction_event_t *e) {
    if (!s_collector || !e->target_is_ours || e->target_rowid <= 0 || e->target_sent_ms <= 0 ||
        !e->target_thread_id || !e->target_thread_id[0])
        return 0;
#ifdef HU_ENABLE_SQLITE
    int64_t sent_ms = 0;
    if (s_collector->db &&
        hu_outbound_sends_repo_find_delivery(s_collector->db, e->channel_id, e->target_thread_id,
                                             strlen(e->target_thread_id), e->target_rowid,
                                             e->target_prev_own_rowid, e->target_sent_ms,
                                             RXN_DELIVERY_SLACK_MS, &sent_ms) == HU_OK)
        return sent_ms;
#endif
    return 0;
}

/* HU_OUTCOME_JOIN != off: land the tapback on its production_outcomes row
 * (LIVE writes; SHADOW only finds the row and logs one aggregate line).
 * Returns the anchored reaction_lookup hit (0/1); the buffers are filled on
 * a hit and the caller uses them only when LIVE. */
static int outcome_join_apply(hu_gate_mode_t mode, const hu_reaction_event_t *e, int exact_hit,
                              char *prompt_buf, size_t prompt_cap, char *response_buf,
                              size_t response_cap, char *alternative_buf, size_t alternative_cap) {
    int64_t delivered_ms = outcome_join_delivery(e);
    int64_t anchor_s = delivered_ms / 1000;
    int anchored_hit = 0;
    if (!exact_hit && delivered_ms > 0)
        anchored_hit =
            reaction_lookup_find_anchored(e, anchor_s, prompt_buf, prompt_cap, response_buf,
                                          response_cap, alternative_buf, alternative_cap);
    int64_t row_id = 0;
    hu_error_t rerr = HU_ERR_NOT_FOUND;
    if (s_collector && e->target_thread_id && (exact_hit || delivered_ms > 0)) {
        int pol = (e->polarity > 0) ? 1 : (e->polarity < 0 ? -1 : 0);
        rerr = hu_dpo_record_tapback(s_collector, e->channel_id, e->target_thread_id,
                                     e->target_message_ref, anchor_s,
                                     HU_DAEMON_REACTIVE_GATE_WINDOW_S, pol,
                                     /*dry_run=*/mode != HU_GATE_LIVE, &row_id);
    }
    if (delivered_ms > 0 && s_engagement_sink)
        s_engagement_sink(e->target_thread_id, strlen(e->target_thread_id), delivered_ms);
    hu_log_info("reaction_handler", NULL,
                "[HU_OUTCOME_JOIN %s] tapback polarity=%d is_from_me_target=%d daemon_sent=%d "
                "exact_hit=%d anchored_hit=%d outcome_row=%d",
                mode == HU_GATE_LIVE ? "live" : "shadow", (int)e->polarity,
                e->target_is_ours ? 1 : 0, delivered_ms > 0 ? 1 : 0, exact_hit, anchored_hit,
                (rerr == HU_OK && row_id > 0));
    return anchored_hit;
}

/* SOTA roadmap #13 (continuity): most recent outbound response for a
 * (channel, thread) pair, independent of msg_ref. See reaction_handler.h. */
int hu_reaction_lookup_last_response(const char *channel, const char *thread, char *out,
                                     size_t out_cap) {
    if (out && out_cap > 0)
        out[0] = '\0';
    if (!channel || !thread || !out || out_cap == 0)
        return 0;
#if HU_RXN_LOOKUP_USES_SQLITE
    if (!rxn_db_open())
        return 0;
    sqlite3_stmt *st = NULL;
    static const char sql[] =
        "SELECT response FROM reaction_lookup WHERE channel = ? AND thread = ? "
        "ORDER BY inserted_at DESC, rowid DESC LIMIT 1";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, thread, -1, SQLITE_STATIC);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *r = sqlite3_column_text(st, 0);
        snprintf(out, out_cap, "%s", r ? (const char *)r : "");
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
#else
    /* In-memory store appends in registration order (upserts rewrite in
     * place), so the LAST matching entry is the most recent registration. */
    for (size_t i = s_lookup_n; i > 0; i--) {
        if (strcmp(s_lookup[i - 1].channel, channel) != 0 ||
            strcmp(s_lookup[i - 1].thread, thread) != 0)
            continue;
        snprintf(out, out_cap, "%s", s_lookup[i - 1].response);
        return 1;
    }
    return 0;
#endif
}

hu_error_t hu_reaction_handler_handle_event(const hu_reaction_event_t *e) {
    if (!e || !e->channel_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (e->is_removal)
        return HU_OK; /* drop removals; we only record adds */

    /* DEF-8: HU_OUTCOME_JOIN (default OFF -> e is used unchanged). LIVE also
     * reads a custom-emoji tapback's polarity from its glyph instead of
     * counting every custom emoji (😢 included) as positive. */
    hu_gate_mode_t oj_mode = outcome_join_mode();
    hu_reaction_event_t oj_event;
    if (oj_mode == HU_GATE_LIVE && e->kind == HU_REACTION_KIND_CUSTOM_EMOJI) {
        oj_event = *e;
        oj_event.polarity = hu_reaction_emoji_polarity(e->emoji);
        e = &oj_event;
    }

#ifdef HU_ENABLE_SQLITE
    /* T8: a thumbs_down (NEGATIVE polarity) is a contradiction signal.
     * Retire the reflection patterns that shaped the thumbed-down turn's
     * system prompt — i.e. those surfaced in this channel within the
     * contradiction window. Fires regardless of lookup_hit: contradiction
     * is about which patterns the turn was built from, not about whether
     * the reaction maps to one of our outbound DPO-tracked messages. */
    if (e->polarity == HU_REACTION_NEGATIVE && s_reflection_db) {
        (void)hu_reflection_retire_contradicted(s_reflection_db, e->channel_id,
                                                HU_REFLECTION_CONTRADICTION_WINDOW_MS,
                                                (uint64_t)time(NULL) * 1000);
    }
#endif

    char prompt_buf[2048];
    char response_buf[4096];
    char alternative_buf[4096];
    prompt_buf[0] = '\0';
    response_buf[0] = '\0';
    alternative_buf[0] = '\0';
    int lookup_hit =
        reaction_lookup_find(e, prompt_buf, sizeof(prompt_buf), response_buf, sizeof(response_buf),
                             alternative_buf, sizeof(alternative_buf));

    /* DEF-8: HU_OUTCOME_JOIN (default OFF -> nothing below runs). */
    if (oj_mode != HU_GATE_OFF) {
        int anchored_hit =
            outcome_join_apply(oj_mode, e, lookup_hit, prompt_buf, sizeof(prompt_buf), response_buf,
                               sizeof(response_buf), alternative_buf, sizeof(alternative_buf));
        if (oj_mode == HU_GATE_LIVE && anchored_hit)
            lookup_hit = 1;
    }

    /* Personal-model ingest fires REGARDLESS of lookup hit. DPO below
     * still requires the lookup (DPO only learns from reactions on OUR
     * outbound messages), but the persona-learning sink wants any
     * observed reaction — contact's reaction on inbound messages is
     * social-graph signal worth recording even without target context.
     *
     * Sprint A.7: if an identity graph is wired, canonicalize the
     * sender_handle BEFORE ingest. HIGH-confidence merges only — the
     * resolver's own conservatism (no display-name-only merges) is what
     * makes this safe to apply automatically. */
    if (s_personal_model) {
        const char *preview = lookup_hit ? response_buf : NULL;

        hu_reaction_event_t effective = *e;
        if (s_identity_graph && e->sender_handle && e->sender_handle[0]) {
            const hu_identity_contact_t *resolved =
                hu_identity_lookup(s_identity_graph, e->sender_handle);
            if (resolved && resolved->merge_confidence >= HU_IDENTITY_CONFIDENCE_HIGH &&
                resolved->canonical_name[0]) {
                /* Rewrite the const pointer to point at the graph's own
                 * canonical_name buffer. The graph outlives this call by
                 * contract (daemon owns it across the loop). */
                effective.sender_handle = resolved->canonical_name;
            }
        }

        (void)hu_reaction_ingest_personal_model(s_personal_model, &effective,
                                                /*custom_emoji=*/effective.emoji, preview,
                                                /*is_from_me_target=*/(lookup_hit != 0),
                                                /*in_group_chat=*/false);
    }

    if (!lookup_hit)
        return HU_ERR_NOT_FOUND;
    if (!s_collector)
        return HU_ERR_NOT_SUPPORTED; /* daemon hasn't wired it yet */

    /* Build source string. hu_preference_pair_t.source is a char[64], so we
     * write into the struct directly (NOT a const char* assignment — that
     * would be a C11 type error since the field is an array, not a pointer). */
    hu_preference_pair_t pair = {0};

    /* Pick source string per channel */
    const char *src = "unknown";
    if (strcmp(e->channel_id, "imessage") == 0)
        src = "imessage_tapback";
    else if (strcmp(e->channel_id, "slack") == 0)
        src = "slack_reactji";
    else
        src = e->channel_id;

    /* Copy strings into fixed-size buffers (NOT pointer assignment — fields
     * are char[2048] / char[4096] / char[64] per include/human/ml/dpo.h:15-26). */
    strncpy(pair.prompt, prompt_buf, sizeof(pair.prompt) - 1);
    pair.prompt_len = strlen(pair.prompt);

    /* Check if we have a complete pair (both sides non-empty) via alternative */
    int has_complete_pair = (alternative_buf[0] != '\0');
    if (has_complete_pair) {
        size_t alt_len = strlen(alternative_buf);
        has_complete_pair = (alt_len >= 4); /* both sides must be >= 4 bytes */
    }

    if (e->polarity > 0) {
        /* Positive reaction → record this response as `chosen` */
        strncpy(pair.chosen, response_buf, sizeof(pair.chosen) - 1);
        pair.chosen_len = strlen(pair.chosen);

        if (has_complete_pair) {
            /* Alternative exists → complete pair: chosen = response, rejected = alternative */
            strncpy(pair.rejected, alternative_buf, sizeof(pair.rejected) - 1);
            pair.rejected_len = strlen(pair.rejected);
        } else {
            /* No alternative or too short → single-sided (behavior unchanged) */
            /* `rejected` left as zeroed-out empty string */
        }
    } else if (e->polarity < 0) {
        /* Negative reaction → record this response as `rejected` */
        strncpy(pair.rejected, response_buf, sizeof(pair.rejected) - 1);
        pair.rejected_len = strlen(pair.rejected);

        if (has_complete_pair) {
            /* Alternative exists → complete pair: rejected = response, chosen = alternative */
            strncpy(pair.chosen, alternative_buf, sizeof(pair.chosen) - 1);
            pair.chosen_len = strlen(pair.chosen);
        } else {
            /* No alternative or too short → single-sided (behavior unchanged) */
            /* `chosen` left as zeroed-out empty string */
        }
    } else {
        return HU_OK; /* neutral reactions don't yield training signal */
    }

    pair.margin = (double)e->polarity;
    pair.timestamp = e->timestamp_unix;
    strncpy(pair.source, src, sizeof(pair.source) - 1);
    pair.source_len = strlen(pair.source);

    /* Set the per-turn flag BEFORE hu_dpo_record_pair so that even if the
     * SQLite insert fails (disk full, schema drift, etc.) the agent_turn
     * code path knows a reaction was observed this turn — the substring
     * heuristic should still defer. The return code is the caller's
     * diagnostic; the flag is the side-effect signal. */
    s_called_this_turn = 1;
    /* LIVE: a changed reaction replaces the earlier pair for this reply. */
    if (oj_mode == HU_GATE_LIVE)
        (void)hu_dpo_forget_reaction_pairs(s_collector, pair.source, pair.prompt, response_buf);
    hu_error_t rec_err = hu_dpo_record_pair(s_collector, &pair);

    /* AGI-C1b — also update the production_outcomes row for this
     * (channel, target, message_ref) with the tapback polarity. This
     * is the "reaction came in" signal that resolves an outbound's
     * outcome columns. Best-effort: failures here don't fail the
     * caller's signal — the per-turn flag is already set. */
    if (oj_mode != HU_GATE_LIVE && e->channel_id && e->target_thread_id && e->target_message_ref) {
        int polarity_int = (e->polarity > 0) ? 1 : (e->polarity < 0 ? -1 : 0);
        (void)hu_dpo_record_outcome(s_collector, e->channel_id, strlen(e->channel_id),
                                    e->target_thread_id, strlen(e->target_thread_id),
                                    e->target_message_ref, strlen(e->target_message_ref),
                                    polarity_int,
                                    /*reply_latency_s=*/-1, /*reply_length=*/-1);
    }
    return rec_err;
}

static void register_assistant_message(const char *channel, const char *thread, const char *msg_ref,
                                       const char *prompt, const char *response,
                                       const char *alternative) {
    if (!channel || !thread || !msg_ref || !prompt || !response)
        return;
#if HU_RXN_LOOKUP_USES_SQLITE
    rxn_db_register(channel, thread, msg_ref, prompt, response, alternative);
#else
    /* In-memory path: overwrite existing entry on key match (upsert
     * semantics so tests can re-register and see the latest values). */
    for (size_t i = 0; i < s_lookup_n; i++) {
        if (strcmp(s_lookup[i].channel, channel) == 0 && strcmp(s_lookup[i].thread, thread) == 0 &&
            strcmp(s_lookup[i].msg_ref, msg_ref) == 0) {
            snprintf(s_lookup[i].prompt, sizeof(s_lookup[i].prompt), "%s", prompt);
            snprintf(s_lookup[i].response, sizeof(s_lookup[i].response), "%s", response);
            snprintf(s_lookup[i].alternative, sizeof(s_lookup[i].alternative), "%s",
                     alternative ? alternative : "");
            s_lookup[i].inserted_at = (int64_t)time(NULL);
            return;
        }
    }
    if (s_lookup_n >= LOOKUP_CAP)
        return;
    snprintf(s_lookup[s_lookup_n].channel, sizeof(s_lookup[0].channel), "%s", channel);
    snprintf(s_lookup[s_lookup_n].thread, sizeof(s_lookup[0].thread), "%s", thread);
    snprintf(s_lookup[s_lookup_n].msg_ref, sizeof(s_lookup[0].msg_ref), "%s", msg_ref);
    snprintf(s_lookup[s_lookup_n].prompt, sizeof(s_lookup[0].prompt), "%s", prompt);
    snprintf(s_lookup[s_lookup_n].response, sizeof(s_lookup[0].response), "%s", response);
    snprintf(s_lookup[s_lookup_n].alternative, sizeof(s_lookup[0].alternative), "%s",
             alternative ? alternative : "");
    s_lookup[s_lookup_n].inserted_at = (int64_t)time(NULL);
    s_lookup_n++;
#endif
}

void hu_reaction_handler_register_assistant_message_for_production(
    const char *channel, const char *thread, const char *msg_ref, const char *prompt,
    const char *response, const char *alternative) {
    register_assistant_message(channel, thread, msg_ref, prompt, response, alternative);
}

#if HU_IS_TEST
void hu_reaction_handler_register_assistant_message_for_test(
    const char *channel, const char *thread, const char *msg_ref, const char *prompt,
    const char *response, const char *alternative) {
    register_assistant_message(channel, thread, msg_ref, prompt, response, alternative);
}
void hu_reaction_handler_reset_for_test(void) {
#if HU_RXN_LOOKUP_USES_SQLITE == 0
    s_lookup_n = 0;
#endif
    s_called_this_turn = 0;
    s_collector = NULL;
    s_personal_model = NULL;
    s_identity_graph = NULL;
    s_reflection_db = NULL;
}

#ifdef HU_ENABLE_SQLITE
int hu_reaction_handler_lookup_db_open_for_test(const char *path) {
    sqlite3 *db = NULL;
    int ok = rxn_db_open_at(path, &db);
    if (db)
        sqlite3_close(db);
    return ok;
}
#endif
#endif
