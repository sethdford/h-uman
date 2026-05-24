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
#include "human/memory/identity_resolver.h"
#include "human/memory/personal_model.h"
#include "human/ml/dpo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
#define HU_RXN_LOOKUP_USES_SQLITE 1
#include <sqlite3.h>
#include <sys/stat.h>
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
 * pattern: daemon sets at init via hu_daemon_reaction_wire_personal_model. */
static hu_personal_model_t *s_personal_model = NULL;
/* Sprint A.7: optional identity-graph wire. NULL == no canonicalization;
 * non-NULL == reactions are looked up via hu_identity_lookup before
 * ingest, and HIGH-confidence merges rewrite sender_handle to the
 * canonical name. */
static const hu_identity_graph_t *s_identity_graph = NULL;

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
#if HU_RXN_LOOKUP_USES_SQLITE

static sqlite3 *s_db = NULL;
static unsigned long s_register_count = 0;
static const unsigned long RXN_CLEANUP_EVERY_N = 1000;
static const long RXN_RETENTION_SECONDS = 60L * 86400L; /* 60 days */

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

static int rxn_db_open(void) {
    if (s_db)
        return 1;

    static char path_buf[1024];
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(path_buf, sizeof(path_buf), "%s/.human/reaction_lookup.db", home);
    rxn_ensure_parent_dir(path_buf);

    if (sqlite3_open(path_buf, &s_db) != SQLITE_OK) {
        if (s_db)
            sqlite3_close(s_db);
        s_db = NULL;
        return 0;
    }
    sqlite3_exec(s_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(s_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    static const char *ddl[] = {
        "CREATE TABLE IF NOT EXISTS reaction_lookup ("
        "channel TEXT NOT NULL,"
        "thread TEXT NOT NULL,"
        "msg_ref TEXT NOT NULL,"
        "prompt TEXT NOT NULL,"
        "response TEXT NOT NULL,"
        "inserted_at INTEGER NOT NULL,"
        "PRIMARY KEY (channel, thread, msg_ref))",
        "CREATE INDEX IF NOT EXISTS idx_reaction_lookup_inserted "
        "ON reaction_lookup(inserted_at DESC)",
        NULL,
    };
    for (size_t i = 0; ddl[i]; i++) {
        if (sqlite3_exec(s_db, ddl[i], NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_close(s_db);
            s_db = NULL;
            return 0;
        }
    }
    return 1;
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
                            const char *prompt, const char *response) {
    if (!rxn_db_open())
        return;

    sqlite3_stmt *st = NULL;
    static const char sql[] = "INSERT OR REPLACE INTO reaction_lookup "
                              "(channel, thread, msg_ref, prompt, response, inserted_at) "
                              "VALUES (?, ?, ?, ?, ?, strftime('%s','now'))";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, thread, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, msg_ref, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, prompt, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, response, -1, SQLITE_STATIC);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);

    /* Periodic retention sweep. Bounded by RXN_CLEANUP_EVERY_N to keep
     * the per-register hot path cheap. */
    if (++s_register_count % RXN_CLEANUP_EVERY_N == 0)
        rxn_db_cleanup_old();
}

/* Lookup returns 1 on hit, 0 on miss. On hit, prompt_out/response_out are
 * filled (truncated via snprintf if needed). */
static int rxn_db_lookup(const char *channel, const char *thread, const char *msg_ref,
                         char *prompt_out, size_t prompt_cap, char *response_out,
                         size_t response_cap) {
    if (!rxn_db_open())
        return 0;

    sqlite3_stmt *st = NULL;
    static const char sql[] = "SELECT prompt, response FROM reaction_lookup "
                              "WHERE channel = ? AND thread = ? AND msg_ref = ? LIMIT 1";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, thread, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, msg_ref, -1, SQLITE_STATIC);

    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(st, 0);
        const unsigned char *r = sqlite3_column_text(st, 1);
        snprintf(prompt_out, prompt_cap, "%s", p ? (const char *)p : "");
        snprintf(response_out, response_cap, "%s", r ? (const char *)r : "");
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

#endif /* HU_RXN_LOOKUP_USES_SQLITE */

/* ===== Unified lookup adapter =====
 *
 * Returns 1 on hit (prompt_out / response_out filled), 0 on miss.
 * Buffers must be sized at least 2048 (prompt) and 4096 (response) to
 * match hu_preference_pair_t's fixed-size columns. */
static int reaction_lookup_find(const hu_reaction_event_t *e, char *prompt_out, size_t prompt_cap,
                                char *response_out, size_t response_cap) {
    const char *thread = e->target_thread_id ? e->target_thread_id : "";
    const char *msg_ref = e->target_message_ref ? e->target_message_ref : "";

#if HU_RXN_LOOKUP_USES_SQLITE
    return rxn_db_lookup(e->channel_id, thread, msg_ref, prompt_out, prompt_cap, response_out,
                         response_cap);
#else
    for (size_t i = 0; i < s_lookup_n; i++) {
        if (strcmp(s_lookup[i].channel, e->channel_id) == 0 &&
            strcmp(s_lookup[i].thread, thread) == 0 && strcmp(s_lookup[i].msg_ref, msg_ref) == 0) {
            snprintf(prompt_out, prompt_cap, "%s", s_lookup[i].prompt);
            snprintf(response_out, response_cap, "%s", s_lookup[i].response);
            return 1;
        }
    }
    return 0;
#endif
}

hu_error_t hu_reaction_handler_handle_event(const hu_reaction_event_t *e) {
    if (!e || !e->channel_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (e->is_removal)
        return HU_OK; /* drop removals; we only record adds */

    char prompt_buf[2048];
    char response_buf[4096];
    prompt_buf[0] = '\0';
    response_buf[0] = '\0';
    int lookup_hit =
        reaction_lookup_find(e, prompt_buf, sizeof(prompt_buf), response_buf, sizeof(response_buf));

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

    if (e->polarity > 0) {
        /* Positive reaction → record this response as `chosen` */
        strncpy(pair.chosen, response_buf, sizeof(pair.chosen) - 1);
        pair.chosen_len = strlen(pair.chosen);
        /* `rejected` left as zeroed-out empty string */
    } else if (e->polarity < 0) {
        /* Negative reaction → record this response as `rejected` */
        strncpy(pair.rejected, response_buf, sizeof(pair.rejected) - 1);
        pair.rejected_len = strlen(pair.rejected);
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
    return hu_dpo_record_pair(s_collector, &pair);
}

static void register_assistant_message(const char *channel, const char *thread, const char *msg_ref,
                                       const char *prompt, const char *response) {
    if (!channel || !thread || !msg_ref || !prompt || !response)
        return;
#if HU_RXN_LOOKUP_USES_SQLITE
    rxn_db_register(channel, thread, msg_ref, prompt, response);
#else
    /* In-memory path: overwrite existing entry on key match (upsert
     * semantics so tests can re-register and see the latest values). */
    for (size_t i = 0; i < s_lookup_n; i++) {
        if (strcmp(s_lookup[i].channel, channel) == 0 && strcmp(s_lookup[i].thread, thread) == 0 &&
            strcmp(s_lookup[i].msg_ref, msg_ref) == 0) {
            snprintf(s_lookup[i].prompt, sizeof(s_lookup[i].prompt), "%s", prompt);
            snprintf(s_lookup[i].response, sizeof(s_lookup[i].response), "%s", response);
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
    s_lookup_n++;
#endif
}

void hu_reaction_handler_register_assistant_message_for_production(const char *channel,
                                                                   const char *thread,
                                                                   const char *msg_ref,
                                                                   const char *prompt,
                                                                   const char *response) {
    register_assistant_message(channel, thread, msg_ref, prompt, response);
}

#if HU_IS_TEST
void hu_reaction_handler_register_assistant_message_for_test(const char *channel,
                                                             const char *thread,
                                                             const char *msg_ref,
                                                             const char *prompt,
                                                             const char *response) {
    register_assistant_message(channel, thread, msg_ref, prompt, response);
}
void hu_reaction_handler_reset_for_test(void) {
#if HU_RXN_LOOKUP_USES_SQLITE == 0
    s_lookup_n = 0;
#endif
    s_called_this_turn = 0;
    s_collector = NULL;
    s_personal_model = NULL;
    s_identity_graph = NULL;
}
#endif
