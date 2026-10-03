#ifndef HU_MEMORY_PROSPECTIVE_REPO_H
#define HU_MEMORY_PROSPECTIVE_REPO_H
/*
 * Prospective memory v2 — the typed intention store (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1).
 *
 * prospective_memories gains typed columns, additively: cue_kind, due_at,
 * status, surfaced_at, attempts, outcome, source. The legacy `fired` column
 * stays authoritative for every pre-v2 reader (the curator, the nightly eval,
 * the HU_PROSPECTIVE=off path); `status` is derived from it — 0 pending,
 * 1 done, 2 canceled, 3 expired — by a trigger whenever a legacy writer
 * changes `fired`, and v2 writers set both columns in one UPDATE.
 * `surfaced` writes fired=0. Every time value is unix SECONDS, like every
 * other column of these tables.
 *
 * Free functions over a borrowed `sqlite3 *db` from hu_sqlite_memory_get_db();
 * domain callers never include sqlite3.h (sqlite-includer ratchet).
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/prospective_policy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Idempotent migration of an existing prospective_memories table: adds the
 * missing v2 columns, maps fired -> status on rows still 'pending', creates
 * idx_prospective_status and the fired -> status trigger. HU_ERR_NOT_FOUND
 * when the table does not exist (the sqlite engine creates it first). */
hu_error_t hu_prospective_repo_ensure_schema(sqlite3 *db);

typedef struct hu_prospective_item {
    int64_t id;
    hu_prospective_cue_kind_t cue_kind;
    hu_prospective_status_t status;
    char trigger_value[256]; /* keyword cue; "commitment:<id>" / "followup:<id>" for time rows */
    char action[512];
    char contact_id[128]; /* "" when the row has no contact */
    int64_t due_at;       /* 0 = none */
    int64_t expires_at;   /* 0 = never */
    int64_t created_at;
    int64_t surfaced_at; /* 0 = never surfaced */
    int attempts;
} hu_prospective_item_t;

/* Rows of `kind` in `status` for `contact`. Keyword reads keep the legacy
 * scope: the contact's rows plus rows with no contact, trigger_type
 * 'keyword' only, newest first. Time reads are the contact's rows, oldest
 * due first. *out holds exactly *out_count items (free with
 * hu_prospective_repo_free), NULL when none. */
hu_error_t hu_prospective_repo_list(hu_allocator_t *alloc, sqlite3 *db,
                                    hu_prospective_cue_kind_t kind, hu_prospective_status_t status,
                                    const char *contact, size_t contact_len,
                                    hu_prospective_item_t **out, size_t *out_count);
void hu_prospective_repo_free(hu_allocator_t *alloc, hu_prospective_item_t *items, size_t count);

/* Move the INTENTION — every row with the item's action, contact and cue
 * kind that is still pending or surfaced — to `to`: status and the matching
 * legacy fired value in one UPDATE, `attempts`, `outcome` (NONE leaves it),
 * and surfaced_at = now when `to` is SURFACED. *changed (may be NULL) is the
 * number of rows updated. */
hu_error_t hu_prospective_repo_transition(sqlite3 *db, const hu_prospective_item_t *it,
                                          hu_prospective_status_t to,
                                          hu_prospective_outcome_t outcome, int attempts,
                                          int64_t now, int *changed);

/* Distinct intentions of `kind` for `contact` with surfaced_at >= since. */
hu_error_t hu_prospective_repo_count_surfaced_since(sqlite3 *db, hu_prospective_cue_kind_t kind,
                                                    const char *contact, size_t contact_len,
                                                    int64_t since, int64_t *out);

/* Open (pending or surfaced) time rows for `contact` with 0 < due_at <= now:
 * the cheap check the proactive tick runs before it loads chat history for a
 * fire-time judge. 0 means a time pass for this contact has nothing to judge,
 * expire or settle. */
hu_error_t hu_prospective_repo_count_due(sqlite3 *db, const char *contact, size_t contact_len,
                                         int64_t now, int64_t *out);

/* Insert a cue_kind='time' row unless this intention is already present,
 * atomically, in one statement: trigger_type 'time', trigger_value =
 * source_key, expires_at = due_at + grace_s (so the legacy sweeps retire it
 * too), status and fired from `status`, created_at = now. "Already present"
 * means either the same source key, OR — for a row still OPEN (status
 * pending or surfaced) — the same contact + the same action once both are
 * trimmed, case-folded and internal whitespace collapsed, IGNORING due_at:
 * a backfill re-run that re-anchors due_at (a fresh source key, a later
 * due_at) must not create a second intention for the same still-open
 * promise. A row that has already settled (done / canceled / expired) does
 * NOT block a fresh intention for the same contact + action. *inserted may
 * be NULL. HU_ERR_INVALID_ARGUMENT for an empty contact/action/key or
 * due_at <= 0. */
hu_error_t hu_prospective_repo_upsert_time(sqlite3 *db, const char *contact, size_t contact_len,
                                           const char *action, size_t action_len, int64_t due_at,
                                           int64_t grace_s, hu_prospective_source_t source,
                                           const char *source_key, hu_prospective_status_t status,
                                           int64_t now, bool *inserted);

/* The time-row mirror of ONE dated ledger row, shared by the live writers
 * (src/memory/superhuman.c) and the backfill (hu_prospective_v2_backfill):
 * hu_prospective_repo_upsert_time with source key "commitment:<ledger_id>"
 * (source promise_keeper) or "followup:<ledger_id>" (source followup) and
 * grace HU_PROSPECTIVE_TIME_GRACE_S. `action` is what
 * hu_prospective_mirror_action decided. *inserted may be NULL. */
hu_error_t hu_prospective_repo_mirror_time(sqlite3 *db, bool is_followup, int64_t ledger_id,
                                           const char *contact, size_t contact_len,
                                           const char *action, size_t action_len, int64_t due_at,
                                           hu_prospective_status_t status, int64_t now,
                                           bool *inserted);

/* One dated ledger row, borrowed for the duration of the callback. */
typedef struct hu_prospective_ledger_row {
    bool is_followup; /* false: commitments row; true: delayed_followups row */
    int64_t id;       /* the ledger row's own id */
    const char *contact;
    size_t contact_len;
    const char *text; /* description / topic, full length */
    size_t text_len;
    const char *who; /* NULL: no ownership signal */
    size_t who_len;
    int64_t due_at; /* deadline / scheduled_at, > 0 */
} hu_prospective_ledger_row_t;

typedef hu_error_t (*hu_prospective_ledger_fn)(void *ctx, const hu_prospective_ledger_row_t *row);

/* Visits every still-open dated ledger row with a contact and text: pending
 * commitments with deadline > 0 (id order), then unsent delayed follow-ups
 * with scheduled_at > 0 (id order). delayed_followups has no `who` column,
 * so a follow-up carries the `who` of a commitment with the same contact and
 * description == topic -- the pair the F20 keeper and the promise keeper
 * write together -- preferring a contact-owned one; a follow-up with no such
 * commitment (the dated-moment path) has no signal (NULL). The callback may
 * write prospective_memories; a non-HU_OK return stops the walk and is
 * returned. */
hu_error_t hu_prospective_repo_each_dated_ledger_row(sqlite3 *db, hu_prospective_ledger_fn fn,
                                                     void *ctx);

/* A settled time intention (DONE / CANCELED / EXPIRED) retires its ledger
 * twins so the legacy readers never resurface it: pending commitments get
 * status 'followed_up' / 'canceled' / 'expired' and followed_up_at = now;
 * unsent delayed_followups get sent=1. The row named by the intention's
 * source key is retired by id (with its F20 twin: same contact, topic ==
 * description). Then (known gap 4) the contact's other still-open ledger
 * rows whose mirror text (hu_prospective_mirror_action: a dated frame's
 * topic, a contact's promise rephrased, else verbatim) normalizes to the
 * intention's action -- the rows the upsert collapsed into it -- are
 * retired too, compared in C. Every by-text/by-topic retire is bounded to
 * rows due <= it->due_at + HU_PROSPECTIVE_TIME_GRACE_S (fix round 1): a
 * same-words promise dated beyond that is a later promise. The earliest
 * such survivor is re-mirrored as a fresh open time row keyed by its own
 * ledger id, with its own due -- call this only once the intention is
 * terminal, as pm_retire does. */
hu_error_t hu_prospective_repo_sync_source(sqlite3 *db, const hu_prospective_item_t *it,
                                           hu_prospective_status_t to, int64_t now);

/* Fix round 2: settle an intention as ONE unit (a SAVEPOINT, so it nests in
 * an open transaction): hu_prospective_repo_transition, then -- for a time
 * intention whose transition changed a row and whose `to` is terminal --
 * hu_prospective_repo_sync_source (ledger retire, bounded sweep, survivor
 * re-mirror). Any failure rolls the whole unit back and is returned;
 * *changed (may be NULL) is then 0. */
hu_error_t hu_prospective_repo_settle(sqlite3 *db, const hu_prospective_item_t *it,
                                      hu_prospective_status_t to, hu_prospective_outcome_t outcome,
                                      int attempts, int64_t now, int *changed);

/* Retire ONE ledger row by its own id, scoped to `contact`, with the same
 * mapping hu_prospective_repo_sync_source uses: a pending commitment gets
 * status 'followed_up' / 'canceled' / 'expired' (DONE / CANCELED / EXPIRED)
 * and followed_up_at = now; an unsent delayed follow-up gets sent=1. A row
 * already retired, of another contact, or missing is left alone. *changed
 * (may be NULL) is the number of rows updated (0 or 1). */
hu_error_t hu_prospective_repo_retire_ledger_row(sqlite3 *db, bool is_followup, int64_t id,
                                                 const char *contact, size_t contact_len,
                                                 hu_prospective_status_t to, int64_t now,
                                                 int *changed);

/* Known gap 2: the legacy path marked delayed follow-up `followup_id` sent
 * (hu_superhuman_delayed_followup_mark_sent -- after an F31 send, or after
 * a send whose proposer context merely LISTED it). Its time twin moves to
 * DONE with NO outcome (the legacy path claims no evidence the reply used
 * it). The twin is a PENDING row of the follow-up's contact that v2 never
 * surfaced (attempts 0, no surfaced_at: v2 owns every row it has surfaced,
 * retries included) and is either keyed -- "followup:<id>", or the F20 key
 * "commitment:<N>" of the same contact's commitment with description ==
 * topic and deadline == scheduled_at, unbounded like every rowid path, so a
 * backfill re-anchored row is still found -- or, for a dated follow-up,
 * has an action equal to the follow-up's mirror text (normalized) and a due
 * <= the follow-up's due + HU_PROSPECTIVE_TIME_GRACE_S. The settle is the
 * same unit a v2 settle is (SAVEPOINT): the bounded sweep re-mirrors a
 * later-dated same-action ledger row as its own open time row; the ledger
 * itself is left as the legacy path wrote it, so what it sends is
 * unchanged. Idempotent; no twin is HU_OK with nothing written. *changed
 * (may be NULL) is the number of rows moved. */
hu_error_t hu_prospective_repo_settle_followup_twin(sqlite3 *db, int64_t followup_id, int64_t now,
                                                    int *changed);

/* Known gap 7: does v2 own legacy ledger row `ledger_id` (a delayed
 * follow-up when `is_followup`, else a commitment)? True when an OPEN
 * (pending or surfaced -- retries included) time row of the row's contact
 * is its twin, by the same identification the legacy settle uses: keyed by
 * the row's own key ("followup:<id>" / "commitment:<id>"), or by its F20
 * partner's key (same contact, description == topic, deadline ==
 * scheduled_at), or -- for a dated row -- an action equal to the row's
 * mirror text (normalized) with a due <= the row's due +
 * HU_PROSPECTIVE_TIME_GRACE_S. Unlike the settle, ANY open row counts: the
 * question is who raises the topic next, and with HU_PROSPECTIVE_TIME=live
 * that is v2 for every open row. A row with only terminal twins, a skipped
 * mirror (never had a twin) or no such ledger row is not owned (*owned
 * false, HU_OK). Read-only. */
hu_error_t hu_prospective_repo_ledger_v2_owned(sqlite3 *db, bool is_followup, int64_t ledger_id,
                                               bool *owned);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_PROSPECTIVE_REPO_H */
