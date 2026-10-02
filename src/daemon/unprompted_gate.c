/* src/daemon/unprompted_gate.c — the one gate stack every unprompted send
 * passes through. Contract and stage order: include/human/daemon/unprompted_gate.h. */
#include "human/daemon/unprompted_gate.h"
#include "human/agent.h"
#include "human/agent/outbound_sanitize.h"
#include "human/agent/proactive.h"
#include "human/autoresponder.h"
#include "human/core/log.h"
#include "human/daemon_contact_optout.h"
#include "human/daemon_proactive.h"
#include "human/memory.h"
#include "human/memory/proactive_decisions_repo.h"
#include "human/security/moderation.h"
#include <string.h>
#include <time.h>

const char *hu_unprompted_kind_str(hu_unprompted_kind_t kind) {
    switch (kind) {
    case HU_UNPROMPTED_PROACTIVE:
        return "proactive";
    case HU_UNPROMPTED_CRON:
        return "cron";
    case HU_UNPROMPTED_BUMP:
        return "bump";
    case HU_UNPROMPTED_F25:
        return "f25";
    case HU_UNPROMPTED_PHOTO:
        return "photo";
    default:
        return "none";
    }
}

const char *hu_unprompted_trigger(hu_unprompted_kind_t kind) {
    switch (kind) {
    case HU_UNPROMPTED_PROACTIVE:
        return "proactive_send";
    case HU_UNPROMPTED_CRON:
        return "unprompted_cron";
    case HU_UNPROMPTED_BUMP:
        return "unprompted_bump";
    case HU_UNPROMPTED_F25:
        return "unprompted_f25";
    case HU_UNPROMPTED_PHOTO:
        return "unprompted_photo";
    default:
        return NULL;
    }
}

const char *hu_unprompted_reason_str(hu_unprompted_reason_t reason) {
    switch (reason) {
    case HU_UNPROMPTED_ALLOW:
        return "none";
    case HU_UNPROMPTED_DENY_INVALID:
        return "invalid";
    case HU_UNPROMPTED_DENY_OPTOUT:
        return "contact_optout";
    case HU_UNPROMPTED_DENY_GOVERNOR:
        return "governor_gated";
    case HU_UNPROMPTED_DENY_COOLOFF:
        return "governor_cooloff";
    case HU_UNPROMPTED_DENY_CAP:
        return "send_cap";
    case HU_UNPROMPTED_DENY_NO_LEDGER:
        return "cap_unverifiable";
    case HU_UNPROMPTED_DENY_RATE_LIMIT:
        return "rate_limited";
    case HU_UNPROMPTED_DENY_QUIET_HOURS:
        return "quiet_hours";
    case HU_UNPROMPTED_DENY_CIRCUIT:
        return "send_circuit_open";
    case HU_UNPROMPTED_DENY_UNREACHABLE:
        return "unreachable";
    case HU_UNPROMPTED_DENY_SANITIZER:
        return "sanitize_refused";
    case HU_UNPROMPTED_DENY_MODERATION:
        return "moderation_flagged";
    }
    return "unknown";
}

void hu_unprompted_gate_init(hu_unprompted_gate_t *g, hu_allocator_t *alloc, struct hu_agent *agent,
                             hu_proactive_budget_t *gov_budget, hu_proactive_throttle_t *throttle,
                             const struct hu_autoresponder_config *ar_cfg, int32_t tz_offset_s,
                             const char *channel_name, const char *target, size_t target_len) {
    if (!g)
        return;
    memset(g, 0, sizeof(*g));
    g->alloc = alloc;
    g->agent = agent;
#ifdef HU_ENABLE_SQLITE
    if (agent && agent->memory)
        g->db = hu_sqlite_memory_get_db(agent->memory);
#endif
    g->gov_budget = gov_budget;
    g->throttle = throttle;
    g->ar_cfg = ar_cfg;
    g->tz_offset_s = tz_offset_s;
    g->channel_name = channel_name;
    g->target = target;
    g->target_len = target_len;
}

static bool unprompted_in_sleep_floor(int64_t now, int32_t tz_offset_s) {
    time_t local = (time_t)(now + tz_offset_s);
    struct tm tm;
    if (!gmtime_r(&local, &tm))
        return true; /* cannot tell the hour: a sleep floor fails closed */
    return tm.tm_hour >= HU_UNPROMPTED_SLEEP_START_H || tm.tm_hour < HU_UNPROMPTED_SLEEP_END_H;
}

/* Stage 2b: THIS contact's cool-off, from the persisted log. Mirrors the
 * old global escalation: the send that made n unanswered was spaced by
 * hu_proactive_backoff_hours(n - 1) (2 → 144h, 3 → 288h, 4+ → never). */
static bool unprompted_cooloff_active(struct sqlite3 *db, const char *contact, int64_t now) {
#ifdef HU_ENABLE_SQLITE
    int64_t n = 0, last = 0;
    if (hu_proactive_decisions_repo_unanswered(db, contact, now, &n, &last) != HU_OK)
        return true; /* cannot read the ledger: fail closed */
    if (n < HU_UNPROMPTED_COOLOFF_AFTER)
        return false;
    uint32_t hours = hu_proactive_backoff_hours((uint32_t)(n - 1));
    if (hours == UINT32_MAX)
        return true;
    return now < last + (int64_t)hours * 3600;
#else
    (void)db;
    (void)contact;
    (void)now;
    return true;
#endif
}

/* Stage 3: the per-contact cap, counted from the persisted log. */
static hu_unprompted_reason_t unprompted_cap(struct sqlite3 *db, const char *contact, int64_t now) {
#ifdef HU_ENABLE_SQLITE
    int64_t day = 0, week = 0;
    if (hu_proactive_decisions_repo_unprompted_sent_since(db, contact, now - 86400, &day) !=
            HU_OK ||
        hu_proactive_decisions_repo_unprompted_sent_since(db, contact, now - 7 * 86400, &week) !=
            HU_OK)
        return HU_UNPROMPTED_DENY_NO_LEDGER;
    if (day >= HU_UNPROMPTED_DAILY_CAP || week >= HU_UNPROMPTED_WEEKLY_CAP)
        return HU_UNPROMPTED_DENY_CAP;
    return HU_UNPROMPTED_ALLOW;
#else
    (void)db;
    (void)contact;
    (void)now;
    return HU_UNPROMPTED_DENY_NO_LEDGER;
#endif
}

static hu_unprompted_reason_t unprompted_policy(const hu_unprompted_gate_t *g, const char *contact,
                                                int64_t now, bool at_send) {
    /* 1. opt-out — consent first; no ledger means nothing to consult. */
#ifdef HU_ENABLE_SQLITE
    if (g->db && hu_contact_optout_enabled() && hu_contact_optout_is_suppressed_db(g->db, contact))
        return HU_UNPROMPTED_DENY_OPTOUT;
#endif
    if (!g->db)
        return HU_UNPROMPTED_DENY_NO_LEDGER; /* fail closed before any later stage */

    /* 2. governor: global ceiling, then this contact's cool-off. */
    if (g->gov_budget && !hu_governor_has_budget(g->gov_budget, (uint64_t)now * 1000ULL))
        return HU_UNPROMPTED_DENY_GOVERNOR;
    if (unprompted_cooloff_active(g->db, contact, now))
        return HU_UNPROMPTED_DENY_COOLOFF;

    /* 3. throttle: persisted per-contact cap; channel bucket only at send. */
    hu_unprompted_reason_t cap = unprompted_cap(g->db, contact, now);
    if (cap != HU_UNPROMPTED_ALLOW)
        return cap;
    if (at_send && g->throttle &&
        !hu_proactive_throttle_channel_try_consume(g->throttle, g->channel_name))
        return HU_UNPROMPTED_DENY_RATE_LIMIT;

    /* 4. quiet hours: static sleep floor, then the operator's DND window. */
    if (unprompted_in_sleep_floor(now, g->tz_offset_s))
        return HU_UNPROMPTED_DENY_QUIET_HOURS;
    if (hu_daemon_proactive_should_skip_for_quiet_hours(g->ar_cfg, now, g->tz_offset_s))
        return HU_UNPROMPTED_DENY_QUIET_HOURS;

    /* 5. circuit breaker. */
#ifdef HU_ENABLE_SQLITE
    if (hu_proactive_send_circuit_is_open(g->db, contact, now))
        return HU_UNPROMPTED_DENY_CIRCUIT;
#endif

    /* 6. reachability (HU_PROACTIVE_REACHABILITY; OFF/SHADOW never deny). */
    if (g->target && hu_daemon_proactive_reach_should_skip(g->agent, g->alloc, g->channel_name,
                                                           contact, g->target, g->target_len))
        return HU_UNPROMPTED_DENY_UNREACHABLE;
    return HU_UNPROMPTED_ALLOW;
}

hu_unprompted_reason_t hu_unprompted_send_check(const hu_unprompted_gate_t *g, const char *contact,
                                                hu_unprompted_kind_t kind, int64_t now, char *text,
                                                size_t *text_len_io, bool at_send) {
    hu_unprompted_reason_t r = HU_UNPROMPTED_DENY_INVALID;
    if (g && contact && contact[0] && kind != HU_UNPROMPTED_NONE)
        r = unprompted_policy(g, contact, now, at_send);

    if (r == HU_UNPROMPTED_ALLOW && at_send && text && text_len_io) {
        /* 7. sanitizer (strips U+FFFC in place; rejects directive echoes). */
        const char *why = NULL;
        if (!hu_outbound_sanitize(text, text_len_io, &why))
            r = HU_UNPROMPTED_DENY_SANITIZER;
        /* 8. moderation — blocking (it was log-only on the cron path). */
        if (r == HU_UNPROMPTED_ALLOW) {
            hu_moderation_result_t mod;
            memset(&mod, 0, sizeof(mod));
            hu_error_t merr = hu_moderation_check(g->alloc, text, *text_len_io, &mod);
            if (merr != HU_OK || mod.flagged)
                r = HU_UNPROMPTED_DENY_MODERATION;
        }
    }

    if (r != HU_UNPROMPTED_ALLOW || at_send)
        hu_log_info("human", (g && g->agent) ? g->agent->observer : NULL,
                    "[unprompted] kind=%s result=%s reason=%s stage=%s",
                    hu_unprompted_kind_str(kind), r == HU_UNPROMPTED_ALLOW ? "allow" : "deny",
                    hu_unprompted_reason_str(r), at_send ? "send" : "pre");
    return r;
}

void hu_unprompted_record_sent(const hu_unprompted_gate_t *g, const char *contact,
                               hu_unprompted_kind_t kind, int64_t now) {
    if (!g || !contact || kind == HU_UNPROMPTED_PROACTIVE || kind == HU_UNPROMPTED_NONE)
        return;
#ifdef HU_ENABLE_SQLITE
    if (g->db) {
        hu_error_t err = hu_proactive_decisions_repo_record(
            g->db, now, contact, hu_unprompted_trigger(kind), HU_PROACTIVE_DECISION_SEND, NULL, 1,
            /*message_ref=*/NULL);
        if (err != HU_OK)
            hu_log_warn("human", g->agent ? g->agent->observer : NULL,
                        "[unprompted] kind=%s ledger write failed (err=%d)",
                        hu_unprompted_kind_str(kind), (int)err);
    }
#endif
    if (g->gov_budget)
        (void)hu_governor_record_sent(g->gov_budget, (uint64_t)now * 1000ULL);
}

void hu_unprompted_record_inbound(struct hu_agent *agent, hu_proactive_budget_t *gov_budget,
                                  const char *contact, size_t contact_len, int64_t now) {
    /* Global counters reset as before: they only feed the reciprocity
     * multiplier now — the cool-off itself is per contact (below). */
    if (gov_budget)
        (void)hu_governor_record_response(gov_budget);
#ifdef HU_ENABLE_SQLITE
    if (!agent || !agent->memory || !contact || contact_len == 0 || contact_len >= 128)
        return;
    struct sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return;
    char who[128];
    memcpy(who, contact, contact_len);
    who[contact_len] = '\0';
    (void)hu_proactive_decisions_repo_record_inbound(db, who, now);
#else
    (void)agent;
    (void)contact;
    (void)contact_len;
    (void)now;
#endif
}
