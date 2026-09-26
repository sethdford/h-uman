#ifndef HU_DAEMON_PROACTIVE_H
#define HU_DAEMON_PROACTIVE_H

#include "agent/governor.h" /* hu_proactive_budget_t (send_and_record) */
#include "core/allocator.h"
#include "core/error.h"
#include "daemon.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct hu_agent;
struct hu_contact_profile;
struct hu_autoresponder_config;
struct hu_legacy_memory;
struct hu_memory_vtable;

/**
 * daemon_proactive.h — Proactive check-in subsystem extracted from daemon.c.
 *
 * Manages contact activity tracking (LRU cache), proactive route parsing,
 * prompt construction, and the main check-in orchestration loop.
 */

/* ── Contact activity LRU cache ─────────────────────────────────────── */

#define HU_DAEMON_CONTACT_ACTIVITY_CAP 256
#define HU_DAEMON_ACTIVITY_FRESH_SECS  (48 * 3600)

typedef struct hu_daemon_contact_activity {
    char contact_id[128];
    char last_channel[64];
    char last_session_key[128];
    time_t last_activity;
    uint64_t lru_seq;
} hu_daemon_contact_activity_t;

/**
 * hu_proactive_context_t — Module state for proactive check-in subsystem.
 *
 * Thread safety: NOT THREAD SAFE. Caller must ensure single-threaded access.
 *   All mutating functions (reset, record, apply_route) assert non-reentrancy
 *   in debug builds.
 *
 * Lifecycle:
 *   - Init:  zero-init with memset or = {0}
 *   - Clear: hu_proactive_context_reset() zeros all fields
 *   - Bounds: entries[] is a fixed-size LRU cache (HU_DAEMON_CONTACT_ACTIVITY_CAP)
 */
typedef struct hu_proactive_context {
    hu_daemon_contact_activity_t entries[HU_DAEMON_CONTACT_ACTIVITY_CAP];
    size_t count;
    uint64_t seq;
} hu_proactive_context_t;

/** Zero all fields in the proactive context (reset the LRU cache). */
void hu_proactive_context_reset(hu_proactive_context_t *ctx);

/** Record inbound activity for a contact on a given channel/session. */
void hu_daemon_contact_activity_record(hu_proactive_context_t *ctx, const char *contact_id,
                                       const char *channel_name, const char *session_key);

/** Check if a channel name exists in the service channel list. */
bool hu_daemon_channel_list_has_name(const hu_service_channel_t *channels, size_t channel_count,
                                     const char *name);

/** Parse proactive_channel string into channel and target route buffers. */
void hu_daemon_proactive_parse_route(const struct hu_contact_profile *cp, char *ch_buf,
                                     char *target_buf);

/** Apply recent activity routing override if contact has fresh inbound activity. */
void hu_daemon_proactive_apply_route(hu_proactive_context_t *ctx, const char *contact_id,
                                     time_t now, const hu_service_channel_t *channels,
                                     size_t channel_count, char *ch_buf, char *target_buf,
                                     size_t *target_len);

/** Build memory callback context for a contact (recalls + degradation + protective filter).
 *  Used by both proactive prompt builder and daemon main loop. */
char *hu_daemon_build_callback_context(hu_allocator_t *alloc, struct hu_legacy_memory *memory,
                                       const char *session_id, size_t session_id_len,
                                       const char *msg, size_t msg_len, size_t *out_len,
                                       struct hu_agent *agent);

/** Build proactive prompt for a contact with memory context, weather, feeds, calendar. */
char *hu_daemon_proactive_prompt_for_contact(hu_allocator_t *alloc, struct hu_agent *agent,
                                             struct hu_legacy_memory *memory,
                                             const struct hu_contact_profile *cp, size_t *out_len);

/* P2-5 (2026-05-16): outbound-safety predicate. Returns false when content
 * is unsafe to inject into an outbound proactive prompt (first-person
 * pronouns, confession verbs, charged emotion keywords, format-specifier
 * injection, newlines). See src/daemon_proactive.c for the contract. */
bool hu_daemon_callback_content_is_safe(const char *content, size_t content_len);

/* Forward declare so we don't pull autoresponder.h into every translation
 * unit that includes daemon_proactive.h. */
struct hu_autoresponder_config;

/* Sprint 41 (2026-05-26 Jordan incident) — quiet-hour gate for proactive
 * outbound sends. Returns true when the daemon proactive-send block MUST
 * skip the send because the recipient is inside the autoresponder's
 * configured DND/quiet window.
 *
 * The previous proactive-send path consulted only rate-limit and per-
 * contact recency throttles; it did NOT consult the autoresponder quiet
 * hours, so "do not disturb 22:00-08:00" was silently ignored for any
 * message originated by the proactive subsystem (the rest of the system
 * — autoresponder, init_proposer — correctly honored it).
 *
 * Pure predicate — extracted so tests can exercise the truth table
 * without spinning a full daemon (see security-predicate-extraction.md
 * rule). Mirrors the same shape init_proposer uses at line 82.
 *
 * Returns false (do NOT skip) when ar_cfg is NULL — operator deliberately
 * disabling quiet hours is a config decision, not a daemon bug. */
bool hu_daemon_proactive_should_skip_for_quiet_hours(const struct hu_autoresponder_config *ar_cfg,
                                                     int64_t now_unix, int32_t tz_offset_seconds);

/* Forward declare governor type. */
struct hu_proactive_budget;

/* Sprint 41 (2026-05-26 follow-up) — daily-budget gate for proactive
 * outbound sends. Parity with init_proposer.c:91 which already enforces
 * this check on the initiative-layer path; daemon_proactive previously
 * called `hu_governor_record_sent` AFTER sending but never consulted
 * `hu_governor_has_budget` BEFORE sending, so a misconfigured budget
 * could be silently exceeded.
 *
 * Pure predicate (extracted per security-predicate-extraction.md):
 *   - Returns true  → MUST skip the send (budget exhausted for this
 *                     wall-clock day).
 *   - Returns false → budget remains OR budget is NULL (operator opted
 *                     out of budget enforcement; mirrors init_proposer's
 *                     NULL-budget semantics).
 *
 * `now_ms` is milliseconds since epoch — same units `hu_governor_record_sent`
 * already takes inside the daemon, so the caller passes the existing
 * `(uint64_t)now * 1000ULL` expression. */
bool hu_daemon_proactive_should_skip_for_budget(struct hu_proactive_budget *budget,
                                                uint64_t now_ms);

/* Forward declare throttle type. */
struct hu_proactive_throttle;

/* Follow-up watcher flush function (US-48-3). Generates a follow-up draft
 * for a contact and sends it via iMessage if throttle allows. Called by
 * hu_daemon_tick_follow_up_watcher() when a scheduled follow-up is ready. */
hu_error_t hu_daemon_follow_up_flush_for_contact(hu_allocator_t *alloc, struct hu_agent *agent,
                                                 const char *contact_handle, struct hu_config *cfg,
                                                 hu_service_channel_t *channels,
                                                 size_t channel_count,
                                                 struct hu_proactive_throttle *throttle);

/* Sprint 59 Phase C (2026-05-26 Annie/Mindy/Betty incident) — per-contact
 * scope for proactive bring-up feed items. The previous call site at the
 * FEED AWARENESS context block in hu_daemon_proactive_prompt_for_contact
 * used hu_feed_processor_get_all_recent, which returns items from EVERY
 * contact. Combined with the emotional-state recorder writing topics keyed
 * by memory_session_id (== the proactive target's contact_id, not the
 * original speaker's), one "lonely" topic in Mindy's feed seeded three
 * rows in emotional_moments — for Mindy, Betty, AND Annie.
 *
 * Pure helper extracted per security-predicate-extraction.md so the scope
 * guarantee can be pinned by tests without spinning up a full agent +
 * persona + memory fixture. Takes a typed hu_contact_profile_t (not a raw
 * const char *) so callers can't accidentally pass a session_id or other
 * identifier-shaped string.
 *
 * Returns:
 *   HU_OK with *out == NULL, *out_count == 0  → contact has no feed items
 *   HU_OK with *out != NULL, *out_count > 0   → caller frees via
 *                                                hu_feed_items_free
 *   HU_ERR_INVALID_ARGUMENT                    → null args (including
 *                                                cp->contact_id == NULL)
 *   propagated SQLite errors                   → query failed
 */
#ifdef HU_ENABLE_SQLITE
#include "feeds/processor.h"
#include <sqlite3.h>
hu_error_t hu_daemon_proactive_get_contact_feed_items(hu_allocator_t *alloc, sqlite3 *db,
                                                      const struct hu_contact_profile *cp,
                                                      size_t limit, hu_feed_item_stored_t **out,
                                                      size_t *out_count);
#endif

/* Run the full proactive gate chain and, if every gate passes, send.
 *
 * Carved out of hu_service_run_proactive_checkins (daemon.c) on 2026-09-20 —
 * 103 lines that sat between the LLM draft and the post-send bookkeeping.
 * Behaviour is unchanged; what is new is that a proposal suppressed by any
 * gate now leaves a proactive_decisions row naming the gate (see
 * hu_daemon_proactive_record_decline), where before it reached only the
 * service log and eval_when_to_speak.py counted it as `dropped_pre_send`
 * with no way to say why (89 of 115 fires, 2026-09-20).
 *
 * `response` is mutated IN PLACE (validator, complexity variation,
 * trailing-period strip, sanitizer) and `*response_len` is updated to match —
 * the caller frees the buffer with that length, exactly as the inline code
 * did. `ar_cfg`, `tz_offset_s` and `throttle` are passed in because the
 * daemon.c helpers that produce them are static there.
 *
 * Returns true iff the channel accepted delivery. Post-send bookkeeping
 * (important-date ring, commitments, jokes, delayed follow-ups) is the
 * caller's, gated on that return. */
bool hu_daemon_proactive_gate_and_send(struct hu_agent *agent, hu_allocator_t *alloc,
                                       hu_channel_t *channel, const struct hu_contact_profile *cp,
                                       const char *ch_name, const char *target, size_t target_len,
                                       char *response, size_t *response_len, int64_t now,
                                       hu_proactive_budget_t *gov_budget,
                                       const struct hu_autoresponder_config *ar_cfg,
                                       int32_t tz_offset_s, hu_proactive_throttle_t *throttle);

/* Send a proactive check-in and record it ONLY if the channel accepted it.
 * Returns true when the message was actually accepted for delivery; false means
 * nothing was sent and NO bookkeeping (send-recency, proactive outcome, governor
 * budget) was performed. Callers must treat false as a non-send. */
bool hu_daemon_proactive_send_and_record(struct hu_agent *agent, hu_channel_t *channel,
                                         const struct hu_contact_profile *cp, const char *ch_name,
                                         const char *target, size_t target_len, const char *message,
                                         size_t message_len, int64_t now,
                                         hu_proactive_budget_t *gov_budget);

/* Record a proactive check-in that was DECLINED before any channel send was
 * attempted — the gates between the proposer firing and the send call
 * (boundary, governor, reactive-recency, validator, throttle, sanitizer).
 *
 * Why this exists (2026-09-20): `scripts/eval_when_to_speak.py` measures the
 * FIR/MIR calibration of the proactive policy, and reported
 * `fir_dropped_pre_send=89` against only 10 delivered sends — 89 FIRED
 * proposals died in those gates with NO row saying which one. They log to the
 * service log, which nothing reads, and are invisible to the metric. With no
 * attribution the script cannot tell "policy correctly stayed quiet" from
 * "a rate-limiter ate it", and FIR has no denominator to work with.
 *
 * `reason` is a short stable slug (e.g. "rate_limited", "sanitize_refused") —
 * it is grouped in SQL, so keep it a closed vocabulary, not free prose.
 * Best-effort and never fails the tick: this is telemetry, not a gate.
 * Do NOT call this after hu_daemon_proactive_send_and_record returns false —
 * that path records its own outcome row and would double-count. */
void hu_daemon_proactive_record_decline(struct hu_agent *agent, const char *contact,
                                        const char *reason, int64_t now);

/* ── Proactive reachability pre-filter (2026-09-20) ──────────────────────
 *
 * Why: docs/plans/2026-09-20-october-roadmap.md O3. Attributing pre-send
 * drops showed 91% of proactive fires (2026-09-20) targeted ONE contact the
 * iMessage blue_guard then HELD as not-iMessage-reachable. Every such fire
 * burns a proposer LLM call, lands in the FIR numerator, and can never be
 * delivered. The fix is to ask the SAME predicate blue_guard asks — before
 * the proposer runs — and leave that contact out of the candidate set.
 *
 * Gate: HU_PROACTIVE_REACHABILITY = off (default) | shadow | live. Unrecognised
 * values fail closed to OFF (same idiom as HU_REPLY_DELAY_MODEL). SHADOW
 * probes and logs "would-exclude" but changes nothing; LIVE skips the
 * proposer for that contact. Promotion to LIVE is gated on a measurement:
 * scripts/eval_when_to_speak.py FIR at n≥30 fires must be not-worse than the
 * pre-LIVE reading (feature-gate-requires-measurement.md).
 *
 * Only iMessage has a reachability oracle (chat.db + whois); other channels
 * always PASS. Builds without HU_HAS_IMESSAGE always PASS. */
typedef enum hu_proactive_reach_mode {
    HU_PROACTIVE_REACH_OFF = 0,
    HU_PROACTIVE_REACH_SHADOW,
    HU_PROACTIVE_REACH_LIVE,
} hu_proactive_reach_mode_t;

typedef enum hu_proactive_reach_action {
    HU_PROACTIVE_REACH_PASS = 0,   /* run the proposer as before */
    HU_PROACTIVE_REACH_WOULD_SKIP, /* SHADOW: log, then run the proposer as before */
    HU_PROACTIVE_REACH_SKIP,       /* LIVE: do not run the proposer for this contact */
} hu_proactive_reach_action_t;

/* Parse HU_PROACTIVE_REACHABILITY. Fail-closed: unset/"off"/junk ⇒ OFF. */
hu_proactive_reach_mode_t hu_daemon_proactive_reach_mode_from_env(void);

/* Pure decision: mode × reachable → action. Extracted so the truth table is
 * pinned without a daemon (security-predicate-extraction.md). */
hu_proactive_reach_action_t hu_daemon_proactive_reach_decide(hu_proactive_reach_mode_t mode,
                                                             bool reachable);

/* Probe (iMessage only) + decide + log. Returns true iff the caller MUST skip
 * the proposer for this contact this tick — i.e. only LIVE and unreachable.
 * SHADOW logs the would-exclude line (grep "proactive reachability") and
 * returns false. Never writes a proactive_decisions row: a pre-filter changes
 * the candidate set, it is not a decision on a fired proposal, and a row here
 * would inflate the FIR denominator the pre-filter is meant to clean. */
bool hu_daemon_proactive_reach_should_skip(struct hu_agent *agent, hu_allocator_t *alloc,
                                           const char *ch_name, const char *contact_id,
                                           const char *target, size_t target_len);

/* Contract C5, Part A — write one row to the proactive_decisions log.
 *
 * The single writer for every daemon-side subsystem that makes a
 * "should I speak now?" call, parameterized by `trigger` so each policy stays
 * separable in scripts/eval_when_to_speak.py ("proactive_send" for check-ins,
 * "follow_up" for the follow-up watcher). Extracted 2026-09-21 when the
 * watcher landed: the resolve-db-then-record preamble was otherwise copied
 * verbatim into a second file (clone-ratchet).
 *
 *   trigger     — non-NULL subsystem name; the row is dropped if NULL.
 *   decision    — one of HU_PROACTIVE_DECISION_{SEND,DECLINE,DEFER}.
 *   sent        — 1 only when a channel actually accepted delivery.
 *   message     — optional; stored as a bounded 64-byte PREFIX, never in full.
 *   now         — unix SECONDS (the `ts` column's contract), never ms.
 *
 * Best-effort: a logging failure never affects the caller's send outcome.
 * A no-op when built without HU_ENABLE_SQLITE. */
void hu_daemon_record_decision_row(struct hu_agent *agent, const char *trigger, const char *contact,
                                   const char *decision, const char *reason, int sent,
                                   const char *message, size_t message_len, int64_t now);

#endif /* HU_DAEMON_PROACTIVE_H */
