#ifndef HU_DAEMON_UNPROMPTED_GATE_H
#define HU_DAEMON_UNPROMPTED_GATE_H
/*
 * ONE gate stack for every unprompted send (DEF-6/7/9/14, 2026-10-02).
 *
 * Before this, each path that texts a contact first carried its own partial
 * copy of the guards: the 10:00 cron called vtable->send with none of them,
 * the read-no-reply bump / F25 check-in / photo share skipped opt-out and the
 * governor, and the per-contact cap was an in-memory counter charged AFTER
 * the LLM had already approved (47 wasted approvals; ~43 restarts reset it).
 *
 * Stages, always in this order (first deny wins):
 *   1. opt-out          contact asked us to stop (consent)
 *   2. governor         global daily/weekly ceiling, then THIS contact's
 *                       unanswered cool-off (2 unanswered → 144h, 3 → 288h,
 *                       4+ → never; only an inbound from THIS contact resets)
 *   3. throttle         per-contact cap (1 per 24h, 3 per 7d), counted from
 *                       the persisted proactive_decisions log; at send time
 *                       also the channel token bucket
 *   4. quiet hours      static sleep floor 23:00–06:00 local, plus the
 *                       operator's autoresponder DND window
 *   5. circuit breaker  delivery keeps failing to this contact
 *   6. reachability     HU_PROACTIVE_REACHABILITY pre-filter (its own gate)
 *   7. sanitizer        at send time, when there is text
 *   8. moderation       at send time, when there is text — BLOCKING
 *
 * These limits are STATIC by policy (anti-spam / consent / sleep ceilings);
 * adaptive cadence may only sit underneath them. Ungated: these are
 * correctness and safety fixes, not a behaviour rollout.
 */
#include "human/agent/governor.h"
#include "human/agent/proactive_throttle.h"
#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_autoresponder_config;
struct sqlite3;

typedef enum hu_unprompted_kind {
    HU_UNPROMPTED_NONE = 0, /* owner-scheduled / not unprompted — never gated */
    HU_UNPROMPTED_PROACTIVE = 1,
    HU_UNPROMPTED_CRON = 2,
    HU_UNPROMPTED_BUMP = 3,
    HU_UNPROMPTED_F25 = 4,
    HU_UNPROMPTED_PHOTO = 5,
} hu_unprompted_kind_t;

typedef enum hu_unprompted_reason {
    HU_UNPROMPTED_ALLOW = 0,
    HU_UNPROMPTED_DENY_INVALID,     /* no contact / no gate */
    HU_UNPROMPTED_DENY_OPTOUT,      /* stage 1 */
    HU_UNPROMPTED_DENY_GOVERNOR,    /* stage 2: global ceiling */
    HU_UNPROMPTED_DENY_COOLOFF,     /* stage 2: this contact's unanswered cool-off */
    HU_UNPROMPTED_DENY_CAP,         /* stage 3: per-contact cap */
    HU_UNPROMPTED_DENY_NO_LEDGER,   /* stage 3: cap cannot be counted — fail closed */
    HU_UNPROMPTED_DENY_RATE_LIMIT,  /* stage 3: channel token bucket */
    HU_UNPROMPTED_DENY_QUIET_HOURS, /* stage 4 */
    HU_UNPROMPTED_DENY_CIRCUIT,     /* stage 5 */
    HU_UNPROMPTED_DENY_UNREACHABLE, /* stage 6 */
    HU_UNPROMPTED_DENY_SANITIZER,   /* stage 7 */
    HU_UNPROMPTED_DENY_MODERATION,  /* stage 8 */
} hu_unprompted_reason_t;

#define HU_UNPROMPTED_DAILY_CAP     1
#define HU_UNPROMPTED_WEEKLY_CAP    3
#define HU_UNPROMPTED_COOLOFF_AFTER 2
/* Static sleep floor: no unprompted send at local hour >= 23 or < 6. */
#define HU_UNPROMPTED_SLEEP_START_H 23
#define HU_UNPROMPTED_SLEEP_END_H   6

/* Everything the stack reads. Pointers are borrowed. A NULL gov_budget means
 * "no global ceiling configured"; a NULL throttle means "no channel bucket";
 * a NULL ar_cfg means "no DND configured". A NULL db FAILS CLOSED at stage 3:
 * an anti-spam ceiling that cannot count its own sends must not allow one. */
typedef struct hu_unprompted_gate {
    hu_allocator_t *alloc;
    struct hu_agent *agent; /* reachability probe + log observer; may be NULL */
    struct sqlite3 *db;
    hu_proactive_budget_t *gov_budget;
    hu_proactive_throttle_t *throttle;
    const struct hu_autoresponder_config *ar_cfg;
    int32_t tz_offset_s;
    const char *channel_name;
    const char *target;
    size_t target_len;
} hu_unprompted_gate_t;

/* Fill `g`; db comes from agent->memory when it is SQLite-backed. */
void hu_unprompted_gate_init(hu_unprompted_gate_t *g, hu_allocator_t *alloc, struct hu_agent *agent,
                             hu_proactive_budget_t *gov_budget, hu_proactive_throttle_t *throttle,
                             const struct hu_autoresponder_config *ar_cfg, int32_t tz_offset_s,
                             const char *channel_name, const char *target, size_t target_len);

/* Daemon-wired variant: the daemon's own governor budget, channel throttle,
 * autoresponder DND and local tz. Implemented in src/daemon.c (which owns
 * that state). Under HU_IS_TEST the global budget is NULL. */
void hu_daemon_unprompted_gate_init(hu_unprompted_gate_t *g, hu_allocator_t *alloc,
                                    struct hu_agent *agent, const char *channel_name,
                                    const char *target, size_t target_len, int64_t now);

/* Run the stack for one unprompted send to `contact`.
 *
 * at_send=false: the pre-LLM / pre-compose screen (stages 1–6). Run it before
 *   any model call, so a capped or opted-out contact costs no GPU.
 * at_send=true: immediately before the channel send — all stages, consumes a
 *   channel token, and runs stages 7–8 on `text` when it is non-NULL (the
 *   sanitizer may shorten *text_len_io in place).
 *
 * Logs one aggregate line per deny, and per allow at send time:
 *   [unprompted] kind=<k> result=allow|deny reason=<r> stage=pre|send
 * (enums only — never text, names or handles). */
hu_unprompted_reason_t hu_unprompted_send_check(const hu_unprompted_gate_t *g, const char *contact,
                                                hu_unprompted_kind_t kind, int64_t now, char *text,
                                                size_t *text_len_io, bool at_send);

/* After a CONFIRMED delivery of a non-proactive kind: write the
 * proactive_decisions row the cap/cool-off count (trigger unprompted_<kind>,
 * sent=1, no message text) and charge the global governor. The proactive
 * kind is recorded by hu_daemon_proactive_send_and_record instead. */
void hu_unprompted_record_sent(const hu_unprompted_gate_t *g, const char *contact,
                               hu_unprompted_kind_t kind, int64_t now);

/* Inbound from `contact`: reset THAT contact's unanswered count (DEF-6).
 * The global budget's counters are reset as before (they feed the global
 * reciprocity multiplier, not a cool-off). Either pointer may be NULL. */
void hu_unprompted_record_inbound(struct hu_agent *agent, hu_proactive_budget_t *gov_budget,
                                  const char *contact, size_t contact_len, int64_t now);

const char *hu_unprompted_kind_str(hu_unprompted_kind_t kind);
const char *hu_unprompted_reason_str(hu_unprompted_reason_t reason);
/* proactive_decisions.trigger for a delivered send of `kind`. */
const char *hu_unprompted_trigger(hu_unprompted_kind_t kind);

#endif /* HU_DAEMON_UNPROMPTED_GATE_H */
