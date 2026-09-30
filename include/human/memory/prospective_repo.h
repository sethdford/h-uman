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
 * twins so the legacy readers never resurface it: pending commitments with
 * the same contact + description get status 'followed_up' / 'canceled' /
 * 'expired' and followed_up_at = now; unsent delayed_followups with the same
 * contact + topic get sent=1. */
hu_error_t hu_prospective_repo_sync_source(sqlite3 *db, const hu_prospective_item_t *it,
                                           hu_prospective_status_t to, int64_t now);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_PROSPECTIVE_REPO_H */
