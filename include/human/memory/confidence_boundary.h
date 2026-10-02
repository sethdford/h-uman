#ifndef HU_MEMORY_CONFIDENCE_BOUNDARY_H
#define HU_MEMORY_CONFIDENCE_BOUNDARY_H

/* Confidence boundary (HU_CONFIDENCE_BOUNDARY=off|shadow|live, default off).
 *
 * Something one contact told the twin must not surface while it talks to
 * someone else. Every memory item carries a share level and, when private,
 * the contact it came from; the recall paths that can carry another contact's
 * items into a reply prompt drop them in LIVE and only count them in SHADOW.
 * An outbound backstop drops a draft sentence that still names a third
 * party's excluded item. Guide: docs/guides/confidence-boundary.md.
 *
 *   off    — nothing is filtered, counted or logged: today's prompts byte for byte.
 *   shadow — each path logs one counts-only line per event; nothing changes.
 *   live   — the items are left out and the backstop drops the sentence. */

#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>

struct hu_personal_model;
struct hu_heuristic_fact;

typedef enum hu_share_level {
    HU_SHARE_UNSET = 0,             /* not yet stamped; read as private, source unknown */
    HU_SHARE_PRIVATE_TO_SOURCE = 1, /* told by one contact; only that contact sees it */
    HU_SHARE_SHAREABLE = 2,         /* public or general; anyone */
    HU_SHARE_OWNER_SELF = 3,        /* Seth's own fact; anyone */
} hu_share_level_t;

/* The recall paths the boundary filters (index into per-path counters). */
typedef enum hu_cb_path {
    HU_CB_PATH_SEMANTIC = 0, /* memory_loader recall (hybrid engine or v1) */
    HU_CB_PATH_EPISODIC,     /* "## Recent Sessions" summaries */
    HU_CB_PATH_PM_FACTS,     /* personal model "Key facts" + per-contact walk */
    HU_CB_PATH_COMMITMENTS,  /* active commitments block */
    HU_CB_PATH_OUTBOUND,     /* backstop: draft sentences */
    HU_CB_PATH_COUNT,
} hu_cb_path_t;

#define HU_CB_CONTACT_MAX 128

/* HU_CONFIDENCE_BOUNDARY per hu_gate_mode_parse; unset -> OFF. */
hu_gate_mode_t hu_confidence_mode(void);
/* Tests: -1 = read the env again; otherwise a hu_gate_mode_t. */
void hu_confidence_set_mode_for_test(int mode);

const char *hu_cb_path_name(hu_cb_path_t path);

/* Write-time derivation for a memories row (pure). `write_contact` is the
 * contact whose conversation produced a session-less write (NULL when none).
 * Rules, first match wins:
 *   key "_pref:"                         -> OWNER_SELF
 *   session non-empty                    -> PRIVATE(session)
 *   key "agent-promise:<c>:" / "contact:<c>:" / "_ep:<c>" -> PRIVATE(c)
 *   write_contact non-empty              -> PRIVATE(write_contact)
 *   source in the owner channels         -> OWNER_SELF
 *   anything else                        -> PRIVATE(unknown source: "")
 * The contact is copied to out_contact (NUL-terminated, "" when unknown). */
hu_share_level_t hu_confidence_derive_row(const char *key, size_t key_len, const char *session,
                                          size_t session_len, const char *source, size_t source_len,
                                          const char *write_contact, size_t write_contact_len,
                                          char *out_contact, size_t out_cap);

/* Personal-model fact (pure): a stamped handle -> PRIVATE(handle); no handle
 * and an owner channel ("cli", "stdin", ...) -> OWNER_SELF; else PRIVATE(""). */
hu_share_level_t hu_confidence_derive_fact(const struct hu_heuristic_fact *f, char *out_contact,
                                           size_t out_cap);

/* True when the channel name is one only the owner writes through. */
bool hu_confidence_is_owner_channel(const char *channel, size_t channel_len);

/* The boundary rule (pure). False whenever there is no current contact (the
 * owner's CLI or a test): nothing is anyone else's then. Otherwise true for a
 * private (or unstamped) item whose source is unknown or another contact. */
bool hu_confidence_excludes(hu_share_level_t level, const char *source, size_t source_len,
                            const char *current, size_t current_len);

/* Owner bypass. The owner texting the twin from their own handle (self-chat)
 * is the owner, not a third party: every per-path filter keeps everything for
 * that contact, as for HU_SHARE_OWNER_SELF. Memory cannot see the persona, so
 * the daemon registers its owner predicate (hu_daemon_confidence_owner_wire ->
 * hu_share_is_owner over the live persona). fn NULL unregisters; with none
 * registered no contact is the owner. */
typedef bool (*hu_confidence_owner_fn)(const void *ctx, const char *contact, size_t contact_len);
void hu_confidence_set_owner_resolver(hu_confidence_owner_fn fn, const void *ctx);
bool hu_confidence_is_owner_contact(const char *contact, size_t contact_len);

/* ── Per-path filters (no-ops in OFF) ─────────────────────────────────────
 * Each logs one counts-only line per call with candidates in SHADOW/LIVE:
 *   [confidence-boundary shadow] path=<p> considered=N would_exclude=M
 * and notes the excluded items' text in the backstop ledger. */

/* Filter recalled/listed rows by provenance (read back by key from the SQLite
 * engine; derived from the entry when the row is not stored). LIVE frees the
 * excluded entries' fields and shrinks the array to the kept count (so the
 * caller's count-sized free stays exact; NULL when none are kept). Returns the
 * new count (unchanged in OFF/SHADOW). */
size_t hu_confidence_filter_entries(hu_memory_t *mem, hu_allocator_t *alloc, hu_cb_path_t path,
                                    hu_memory_entry_t **entries, size_t count, const char *contact,
                                    size_t contact_len);

/* Personal model as this contact may see it. OFF/SHADOW and nothing to drop:
 * returns `pm` itself (*owned NULL). LIVE with facts to drop: returns a heap
 * copy without them (*owned set; free with hu_confidence_pm_view_free), or
 * NULL when that copy cannot be allocated: fail closed, render no block. */
const struct hu_personal_model *hu_confidence_pm_view(hu_allocator_t *alloc,
                                                      const struct hu_personal_model *pm,
                                                      const char *contact, size_t contact_len,
                                                      struct hu_personal_model **owned);
void hu_confidence_pm_view_free(hu_allocator_t *alloc, struct hu_personal_model *owned);

/* Stamp a just-written, session-less row with the contact whose conversation
 * wrote it (NULL/empty contact: the owner was writing -> OWNER_SELF). Used by
 * writers that store globally (experience, commitments, agent promises). */
void hu_confidence_stamp_write(hu_memory_t *mem, const char *key, size_t key_len,
                               const char *contact, size_t contact_len);

/* ── Outbound backstop ────────────────────────────────────────────────────
 * The filters note each excluded item for the current contact; the backstop
 * checks the draft against them, then clears the ledger. A sentence matches
 * an item when it names one of the item's names (a Capitalized word that is
 * not a stopword, word-bounded, case-insensitive) AND shares one more content
 * word (>= 4 letters) with it. */
void hu_confidence_ledger_note(const char *contact, size_t contact_len, const char *text,
                               size_t text_len);
size_t hu_confidence_ledger_count(void);
void hu_confidence_ledger_clear(void);

/* Pure: how many sentences of `draft` match a ledger item; with `out` non-NULL
 * writes the draft minus those sentences (NUL-terminated, <= draft_len). */
size_t hu_confidence_backstop_scan(const char *draft, size_t draft_len, char *out, size_t *out_len);

/* Turn hook: OFF no-op. SHADOW logs the would-drop count. LIVE replaces
 * *response with the draft minus matching sentences (freeing the old buffer as
 * len + 1). Clears the ledger in SHADOW and LIVE. Returns sentences matched. */
size_t hu_confidence_backstop_apply(hu_allocator_t *alloc, const char *contact, size_t contact_len,
                                    char **response, size_t *response_len);

#endif /* HU_MEMORY_CONFIDENCE_BOUNDARY_H */
