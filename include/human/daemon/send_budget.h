#ifndef HU_DAEMON_SEND_BUDGET_H
#define HU_DAEMON_SEND_BUDGET_H

/* Reactive reply budget — a runaway brake for the reply path.
 *
 * Incident 2026-09-01: a stale-cursor replay produced 17 sends to one contact
 * and ~47 in total inside an hour, and nothing on the reactive path counted
 * them (the channel rate limiter's only consumer is the proactive throttle).
 *
 * Unit is a REPLY (one processed inbound batch), not a bubble: the two-bubble
 * "sorry just saw this" + answer is one reply. Two scopes, each a sliding
 * one-hour window; 0 disables a scope. Deny means the daemon stays silent —
 * a human who went quiet is normal, a bot that never stops is the tell.
 *
 * Not persisted: a fresh process starts with an empty window. The replay
 * guards in src/channels/imessage.c cover the restart case; this covers a
 * runaway inside one process life.
 *
 * The contact key is the daemon's batch/session key, so a GROUP chat counts
 * as one contact: ten daemon replies into one group inside an hour trips the
 * per-contact scope. That is deliberate — a persona that posts ten times an
 * hour into a group is louder than the human it models — and tunable via
 * behavior.reply_budget_per_contact_hourly. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_SEND_BUDGET_DEFAULT_PER_CONTACT 10u
#define HU_SEND_BUDGET_DEFAULT_GLOBAL      30u
#define HU_SEND_BUDGET_WINDOW_SEC          3600
#define HU_SEND_BUDGET_MAX_CONTACTS        128
#define HU_SEND_BUDGET_MAX_PER_CONTACT     32
#define HU_SEND_BUDGET_MAX_GLOBAL          256
#define HU_SEND_BUDGET_KEY_CAP             128

typedef enum hu_send_budget_reason {
    HU_SEND_BUDGET_OK = 0,
    HU_SEND_BUDGET_CONTACT_EXHAUSTED,
    HU_SEND_BUDGET_GLOBAL_EXHAUSTED,
} hu_send_budget_reason_t;

typedef struct hu_send_budget_contact {
    char key[HU_SEND_BUDGET_KEY_CAP];
    int64_t sent_at[HU_SEND_BUDGET_MAX_PER_CONTACT]; /* ring, 0 = empty */
    size_t next;                                     /* ring write index */
    int64_t last_sent;                               /* eviction key */
} hu_send_budget_contact_t;

typedef struct hu_send_budget {
    uint32_t per_contact_hourly; /* 0 = scope disabled */
    uint32_t global_hourly;      /* 0 = scope disabled */
    hu_send_budget_contact_t contacts[HU_SEND_BUDGET_MAX_CONTACTS];
    int64_t global_sent_at[HU_SEND_BUDGET_MAX_GLOBAL];
    size_t global_next;
} hu_send_budget_t;

/* ── Pure core (tests drive this directly) ─────────────────────────── */

void hu_send_budget_init(hu_send_budget_t *b, uint32_t per_contact_hourly, uint32_t global_hourly);

/* Read-only: would a reply to `contact` at `now` be within budget?
 * A NULL/empty contact skips the per-contact scope (never suppress on a bad
 * key); the global scope still applies. `why` may be NULL. */
bool hu_send_budget_allows(const hu_send_budget_t *b, const char *contact, size_t contact_len,
                           int64_t now, hu_send_budget_reason_t *why);

/* Charge one reply at `now`. Call only after a successful send. */
void hu_send_budget_record(hu_send_budget_t *b, const char *contact, size_t contact_len,
                           int64_t now);

/* ── Module singleton (what src/daemon.c calls) ─────────────────────── */

/* Configure limits (from config.json behavior.reply_budget_*). Idempotent. */
void hu_send_budget_configure(uint32_t per_contact_hourly, uint32_t global_hourly);

/* Check the singleton; on deny fills `why`, `used` (replies in the window for
 * the exhausted scope) and `cap` (that scope's limit) when non-NULL. */
bool hu_send_budget_check(const char *contact, size_t contact_len, int64_t now,
                          hu_send_budget_reason_t *why, uint32_t *used, uint32_t *cap);

void hu_send_budget_record_send(const char *contact, size_t contact_len, int64_t now);

/* Clear all history and limits (tests). */
void hu_send_budget_reset(void);

#endif /* HU_DAEMON_SEND_BUDGET_H */
