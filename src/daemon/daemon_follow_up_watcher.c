/* src/daemon/daemon_follow_up_watcher.c
 *
 * US-48-3: Follow-up watcher — daemon subsystem that polls iMessage chat.db
 * for INBOUND messages the user never replied to, and proposes a follow-up
 * for the ones that have gone stale.
 *
 * Direction matters. The LIVE outbound scheduler
 * (src/daemon/daemon_followup_sched.c, wired at daemon.c:794) handles the
 * mirror case — seth wrote, the contact read it and went quiet — via
 * hu_imessage_find_unreplied_read. THIS watcher is the inverse
 * (hu_imessage_find_inbound_unreplied): the contact wrote, seth read it and
 * never answered. The two must not share copy; see the `on`-path notes below.
 *
 * Gate: HU_FOLLOW_UP_WATCHER = off | shadow | on, DEFAULT SHADOW.
 *   off    — return after the config/interval checks; the finder is never
 *            called, so there is zero chat.db cost.
 *   shadow — run detection + the send-now predicate, then log one line and
 *            write ONE proactive_decisions row per contact per UTC day:
 *            trigger='follow_up', decision='defer',
 *            reason='shadow:would_send', sent=0. NO send path is invoked
 *            and the production throttle ledger is NOT charged. 'defer'
 *            rather than 'send' because eval_when_to_speak.py:406 counts
 *            any decision='send' in the window as "not missed" regardless
 *            of the sent column, so a shadow 'send' would deflate MIR.
 *   on     — additionally hand the proposal to the real proactive gate
 *            stack, but ONLY when every prerequisite is present (text
 *            source, gov_budget, ar_cfg); otherwise it degrades to shadow
 *            and names the missing one. Inert in production today: no
 *            direction-correct text source exists (see daemon.h).
 *
 * Exported tick function hu_daemon_tick_follow_up_watcher is called from
 * daemon.c at configurable intervals (default 5 min).
 */

#include "human/agent.h"
#include "human/agent/init_proposer.h"
#include "human/agent/proactive_throttle.h"
#include "human/channels/imessage.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/daemon_proactive.h"
#include "human/follow_up.h"
#include "human/persona.h"

/* Only the decision VOCABULARY (HU_PROACTIVE_DECISION_*, defined
 * unconditionally in this header) is needed here. The sqlite handle itself
 * stays behind hu_daemon_record_decision_row (daemon_proactive.h) — per
 * .claude/rules/sqlite-includer-ratchet.md domain code never touches it. */
#include "human/memory/proactive_decisions_repo.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Per ~/.claude/rules/silent-config-gated-subsystems.md: emit ONE
 * operator-visible log line per process when follow_up_watcher is
 * disabled or enabled. Guards are process-scoped via atomic_bool. */
static atomic_bool g_warned_followup_watcher_disabled = false;
static atomic_bool g_warned_followup_watcher_enabled = false;
static atomic_bool g_warned_followup_watcher_off = false;
static atomic_bool g_warned_followup_watcher_on_inert = false;

/* ── Injected seams (contract in include/human/daemon.h) ──────────────── */

static hu_follow_up_finder_fn g_finder_fn = NULL; /* NULL => built-in default */
static void *g_finder_ctx = NULL;
static hu_follow_up_text_fn g_text_fn = NULL; /* NULL => `on` stays inert */
static void *g_text_ctx = NULL;

/* Observability counters. Incremented unconditionally — these are plain
 * statics, not a behavior fork; only the accessors below are test-only. */
static unsigned g_send_now_calls = 0;
static unsigned g_proposals = 0;

void hu_daemon_follow_up_watcher_set_finder(hu_follow_up_finder_fn fn, void *ctx) {
    g_finder_fn = fn;
    g_finder_ctx = ctx;
}

void hu_daemon_follow_up_watcher_set_text_source(hu_follow_up_text_fn fn, void *ctx) {
    g_text_fn = fn;
    g_text_ctx = ctx;
}

#if HU_IS_TEST
void hu_daemon_follow_up_watcher_reset_warn_guards_for_test(void) {
    atomic_store(&g_warned_followup_watcher_disabled, false);
    atomic_store(&g_warned_followup_watcher_enabled, false);
    atomic_store(&g_warned_followup_watcher_off, false);
    atomic_store(&g_warned_followup_watcher_on_inert, false);
}

unsigned hu_daemon_follow_up_watcher_send_now_calls_for_test(void) {
    return g_send_now_calls;
}

unsigned hu_daemon_follow_up_watcher_proposals_for_test(void) {
    return g_proposals;
}
#endif

/* ── Per-contact, per-UTC-day shadow ledger ───────────────────────────────
 * Static-table shape borrowed from max_tokens_shadow_mark_and_check_seen
 * (src/agent/agent.c:170), keyed by (contact, UTC day).
 *
 * This gates BOTH the shadow log line and the shadow decision row. Without it
 * a contact who stays unreplied yields a row every tick — ~288/day at the
 * 5-minute default — because the send-now predicate runs against a fresh
 * throttle copy each tick and so never self-limits. That would swamp
 * scripts/eval_when_to_speak.py's per-contact decision window with duplicates
 * of a single standing proposal. */

#define HU_FUW_SEEN_CAP     32
#define HU_FUW_SEEN_KEY_CAP 64

static char g_fuw_seen[HU_FUW_SEEN_CAP][HU_FUW_SEEN_KEY_CAP];
static uint32_t g_fuw_seen_ymd[HU_FUW_SEEN_CAP];
static size_t g_fuw_seen_count = 0;

/* YYYYMMDD in UTC. Deliberately UTC, not local: the caller's now_unix is the
 * only clock input, so the bucket stays deterministic across DST shifts and
 * in tests. */
static uint32_t follow_up_utc_ymd(int64_t now_unix) {
    time_t t = (time_t)now_unix;
    struct tm g;
    if (!gmtime_r(&t, &g))
        return 0;
    return (uint32_t)((g.tm_year + 1900) * 10000 + (g.tm_mon + 1) * 100 + g.tm_mday);
}

/* Returns true (SUPPRESS: already recorded for this contact today) or false
 * (first time today — marks it and the caller should log + record). */
static bool follow_up_mark_and_check_seen_today(const char *contact, uint32_t ymd) {
    if (!contact || !contact[0])
        return true;
    size_t n = strlen(contact);
    if (n >= HU_FUW_SEEN_KEY_CAP)
        n = HU_FUW_SEEN_KEY_CAP - 1;
    for (size_t i = 0; i < g_fuw_seen_count; i++) {
        if (strncmp(g_fuw_seen[i], contact, n) != 0 || g_fuw_seen[i][n] != '\0')
            continue;
        if (g_fuw_seen_ymd[i] == ymd)
            return true;         /* already recorded today */
        g_fuw_seen_ymd[i] = ymd; /* new day: let one through, reuse the slot */
        return false;
    }
    if (g_fuw_seen_count < HU_FUW_SEEN_CAP) {
        memcpy(g_fuw_seen[g_fuw_seen_count], contact, n);
        g_fuw_seen[g_fuw_seen_count][n] = '\0';
        g_fuw_seen_ymd[g_fuw_seen_count] = ymd;
        g_fuw_seen_count++;
    }
    return false;
}

/* Names every reason `on` cannot send, so the warn-once is actionable rather
 * than just "not sending". Returns true if any blocker applies. */
static bool follow_up_on_blockers(char *out, size_t cap, bool has_text, bool has_budget,
                                  bool has_ar_cfg) {
    if (!out || cap == 0)
        return true;
    out[0] = '\0';
    if (has_text && has_budget && has_ar_cfg)
        return false;
    snprintf(out, cap, "%s%s%s",
             has_text ? "" : "no follow-up text source is wired (see daemon.h); ",
             has_budget ? ""
                        : "gov_budget is NULL, so a send would be neither limited by nor "
                          "counted against the shared daily proactive budget; ",
             has_ar_cfg ? "" : "ar_cfg is NULL, so autoresponder quiet hours are not enforced; ");
    return true;
}

#if HU_IS_TEST
void hu_daemon_follow_up_watcher_reset_counters_for_test(void) {
    g_send_now_calls = 0;
    g_proposals = 0;
    g_fuw_seen_count = 0;
}
#endif

/* ── Default finder: the real chat.db query ───────────────────────────── */

static hu_error_t follow_up_default_finder(void *ctx, const char *contact_id, size_t contact_id_len,
                                           int64_t *out_msg_id, uint64_t *out_inbound_at_ms) {
    (void)ctx;
#ifdef HU_HAS_IMESSAGE
    return hu_imessage_find_inbound_unreplied(contact_id, contact_id_len, out_msg_id,
                                              out_inbound_at_ms);
#else
    /* Builds without iMessage support have no chat.db to query. The caller
     * already filtered to imessage channels, so this is unreachable in
     * practice; stub out so the symbol isn't required at link time. */
    (void)contact_id;
    (void)contact_id_len;
    if (out_msg_id)
        *out_msg_id = 0;
    if (out_inbound_at_ms)
        *out_inbound_at_ms = 0;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ── Decision log ─────────────────────────────────────────────────────────
 * Writes the row scripts/eval_when_to_speak.py reads. Distinct trigger from
 * the proactive path's 'proactive_send' so the two policies stay separable
 * in the metric. `ts` is unix SECONDS (the column's contract), never ms.
 * Best-effort: a logging failure must never change the send outcome. */
static void follow_up_record_decision(struct hu_agent *agent, const char *contact,
                                      const char *decision, const char *reason, int sent,
                                      int64_t now_unix) {
    /* Shared writer (daemon_proactive.h) rather than a local copy of the
     * resolve-db-then-record preamble. 'follow_up' keeps this policy separable
     * from 'proactive_send' in scripts/eval_when_to_speak.py. No message_ref:
     * shadow has no message, and the live path's text is already prefixed into
     * the row gate_and_send writes. */
    hu_daemon_record_decision_row(agent, "follow_up", contact, decision, reason, sent, NULL, 0,
                                  now_unix);
}

/* ── LIVE proposal hand-off ───────────────────────────────────────────────
 * Never calls channel->vtable->send from this file. The proposal goes to
 * hu_daemon_proactive_gate_and_send, the same arbiter proactive check-ins
 * use: protective boundary, governor (quiet hours + daily budget), reactive
 * deferral, validator chain, outbound sanitizer, channel rate-limit,
 * per-contact send-cap, and only then send_and_record.
 *
 * The O3 lesson (docs/plans/2026-09-20-october-roadmap.md) — budget spent on
 * a contact that could not receive — is handled by running the reachability
 * pre-filter FIRST, mirroring daemon.c:1480. gate_and_send does not apply it
 * itself; it is a separate pre-filter by design (daemon_proactive.h:285).
 *
 * Returns true iff the channel accepted delivery. */
static bool follow_up_gate_and_send(struct hu_agent *agent, hu_channel_t *channel,
                                    const hu_contact_profile_t *cp, const char *ch_name,
                                    hu_proactive_throttle_t *throttle,
                                    hu_proactive_budget_t *gov_budget,
                                    const struct hu_autoresponder_config *ar_cfg, uint64_t age_ms,
                                    int64_t now_unix, int32_t tz_offset_s) {
    /* g_text_fn is checked by the caller, but re-check locally: this function
     * dereferences it, and a future caller must not be able to reach that
     * dereference by forgetting the guard. */
    if (!agent || !agent->alloc || !channel || !cp || !cp->contact_id || !g_text_fn)
        return false;

    const char *target = cp->contact_id;
    size_t target_len = strlen(target);

    /* No decision row here: hu_daemon_proactive_reach_should_skip's contract
     * (daemon_proactive.h:285) is explicit that a pre-filter never writes one —
     * it changes which contacts are CONSIDERED, and logging a decline would
     * make the eval count a policy decision that was never made. */
    if (hu_daemon_proactive_reach_should_skip(agent, agent->alloc, ch_name, cp->contact_id, target,
                                              target_len))
        return false;

    /* Buffer is mutated in place by the validator/sanitizer chain inside
     * gate_and_send, so it must be writable and larger than the text. */
    char response[512];
    response[0] = '\0';
    if (g_text_fn(g_text_ctx, cp->contact_id, age_ms, response, sizeof(response)) != HU_OK ||
        response[0] == '\0') {
        follow_up_record_decision(agent, cp->contact_id, HU_PROACTIVE_DECISION_DECLINE,
                                  "compose_failed", 0, now_unix);
        return false;
    }
    size_t response_len = strlen(response);

    bool sent = hu_daemon_proactive_gate_and_send(agent, agent->alloc, channel, cp, ch_name, target,
                                                  target_len, response, &response_len, now_unix,
                                                  gov_budget, ar_cfg, tz_offset_s, throttle);
    follow_up_record_decision(agent, cp->contact_id,
                              sent ? HU_PROACTIVE_DECISION_SEND : HU_PROACTIVE_DECISION_DECLINE,
                              sent ? NULL : "gated", sent ? 1 : 0, now_unix);
    return sent;
}

/* Everything the per-contact scan needs that is constant across the tick.
 * Bundled so the scan keeps a readable signature as the governor state grew. */
typedef struct follow_up_tick_ctx {
    hu_gate_mode_t mode;
    struct hu_agent *agent;
    hu_channel_t *channel;
    const char *ch_name;
    hu_proactive_throttle_t *throttle;           /* real ledger — live sends only */
    hu_proactive_throttle_t *predicate_throttle; /* per-tick copy — see the tick */
    hu_proactive_budget_t *gov_budget;
    const struct hu_autoresponder_config *ar_cfg;
    int64_t now_unix;
    uint32_t ymd; /* UTC day, for the per-contact-per-day shadow ledger */
    int32_t tz_offset_s;
    bool can_send;        /* mode==LIVE AND every `on` prerequisite is present */
    const char *blockers; /* non-empty when can_send is false in LIVE */
} follow_up_tick_ctx_t;

/* ── Per-contact scan ─────────────────────────────────────────────────────
 * Returns after handling one contact; factored out to keep the tick's
 * nesting shallow. */
static void follow_up_scan_contact(const follow_up_tick_ctx_t *tc, const hu_contact_profile_t *cp) {
    struct hu_agent *agent = tc->agent;
    int64_t now_unix = tc->now_unix;

    if (!cp->contact_id || !cp->contact_id[0] || !cp->warmth_level)
        return;
    /* Same warmth rule the live outbound scheduler applies: acquaintances
     * never get unprompted follow-ups (daemon_followup_sched.c:77). */
    if (hu_followup_warmth_from_string(cp->warmth_level) == HU_FOLLOWUP_WARMTH_NONE)
        return;

    hu_follow_up_finder_fn finder = g_finder_fn ? g_finder_fn : follow_up_default_finder;
    void *finder_ctx = g_finder_fn ? g_finder_ctx : NULL;

    int64_t msg_id = 0;
    uint64_t inbound_at_ms = 0;
    if (finder(finder_ctx, cp->contact_id, strlen(cp->contact_id), &msg_id, &inbound_at_ms) !=
            HU_OK ||
        msg_id == 0)
        return; /* query failed, or seth already replied */

    /* now_ms is derived from the caller's now_unix rather than read from the
     * clock, so the tick stays deterministic and testable — same convention
     * as daemon_followup_sched.c:103. */
    uint64_t now_ms = (uint64_t)now_unix * 1000ULL;
    if (inbound_at_ms == 0 || now_ms <= inbound_at_ms)
        return; /* clock skew or a future timestamp: never a candidate */
    uint64_t age_ms = now_ms - inbound_at_ms;
    if (age_ms < HU_FOLLOW_UP_WATCHER_MIN_AGE_MS)
        return; /* still inside the grace window */

    g_send_now_calls++;
    if (!hu_follow_up_should_send_now(cp->contact_id, now_ms, tc->predicate_throttle)) {
        follow_up_record_decision(agent, cp->contact_id, HU_PROACTIVE_DECISION_DECLINE, "throttled",
                                  0, now_unix);
        return;
    }
    g_proposals++;

    if (tc->can_send) {
        (void)follow_up_gate_and_send(agent, tc->channel, cp, tc->ch_name, tc->throttle,
                                      tc->gov_budget, tc->ar_cfg, age_ms, now_unix,
                                      tc->tz_offset_s);
        return;
    }

    /* SHADOW, or LIVE missing a prerequisite. Say once which. */
    if (tc->mode == HU_GATE_LIVE)
        hu_log_warn_once(&g_warned_followup_watcher_on_inert, "follow_up_watcher",
                         agent ? agent->observer : NULL,
                         "HU_FOLLOW_UP_WATCHER=on but proposals behave as shadow: %s. The repo's "
                         "only follow-up copy (hu_followup_compose_directive, "
                         "hu_followup_decide.template_text) phrases the OUTBOUND case ('<contact> "
                         "read your message and hasn't replied'); this watcher detects the INBOUND "
                         "case (they wrote, seth never answered), where that text is exactly "
                         "backwards. See the flip prerequisites in include/human/daemon.h.",
                         tc->blockers ? tc->blockers : "prerequisite missing");

    /* One shadow row + one log line per contact per UTC day. */
    if (follow_up_mark_and_check_seen_today(cp->contact_id, tc->ymd))
        return;

    unsigned age_hours = (unsigned)(age_ms / 3600000ULL);
    hu_log_info("follow_up_watcher", agent ? agent->observer : NULL,
                "[follow-up-watcher shadow] would follow up %s after %uh", cp->contact_id,
                age_hours);

    /* DEFER, not SEND. scripts/eval_when_to_speak.py:406 counts ANY
     * decision='send' row in the window as "not missed", regardless of the
     * `sent` column — so a shadow row claiming 'send' would mark a genuinely
     * missed opportunity as caught and silently deflate MIR. 'defer' is also
     * the honest word: the proposal fired and was held, not delivered. */
    follow_up_record_decision(agent, cp->contact_id, HU_PROACTIVE_DECISION_DEFER,
                              "shadow:would_send", 0, now_unix);
}

hu_error_t hu_daemon_tick_follow_up_watcher(
    const struct hu_follow_up_watcher_config *cfg, int64_t now_unix, int64_t *last_poll_unix_inout,
    int64_t *watermark_inout, struct hu_agent *agent, const struct hu_config *config,
    hu_service_channel_t *channels, size_t channel_count, struct hu_proactive_throttle *throttle,
    struct hu_proactive_budget *gov_budget, const struct hu_autoresponder_config *ar_cfg) {
    (void)config; /* governor state arrives explicitly as gov_budget / ar_cfg */
    if (!cfg || !last_poll_unix_inout || !watermark_inout)
        return HU_ERR_INVALID_ARGUMENT;

    if (!cfg->enabled) {
        hu_log_info_once(&g_warned_followup_watcher_disabled, "follow_up_watcher", NULL,
                         "follow_up_watcher subsystem disabled by config "
                         "(cfg->follow_up_watcher.enabled=false); set "
                         "follow_up_watcher.enabled=true in config.json to activate");
        return HU_OK;
    }

    hu_log_info_once(&g_warned_followup_watcher_enabled, "follow_up_watcher", NULL,
                     "follow_up_watcher subsystem activated by config "
                     "(cfg->follow_up_watcher.enabled=true)");

    /* Check if enough time has passed since last poll. Default interval is 300s (5 min). */
    int interval = cfg->interval_seconds > 0 ? cfg->interval_seconds : 300;
    if (*last_poll_unix_inout > 0 && now_unix - *last_poll_unix_inout < interval)
        return HU_OK;

    hu_gate_mode_t mode = hu_gate_mode_from_env("HU_FOLLOW_UP_WATCHER", HU_GATE_SHADOW);
    if (mode == HU_GATE_OFF) {
        hu_log_info_once(&g_warned_followup_watcher_off, "follow_up_watcher", NULL,
                         "follow_up_watcher gated OFF by HU_FOLLOW_UP_WATCHER=off; "
                         "chat.db is not queried (unset or set shadow|on to activate)");
        *last_poll_unix_inout = now_unix;
        *watermark_inout = now_unix;
        return HU_OK;
    }

    if (agent && agent->persona && channels && channel_count > 0) {
        time_t now_t = (time_t)now_unix;
        struct tm local_tm;
        int32_t tz_offset_s = 0;
        if (localtime_r(&now_t, &local_tm))
            tz_offset_s = (int32_t)local_tm.tm_gmtoff;

        /* The send-now predicate runs against a COPY of the throttle in every
         * mode, because hu_follow_up_should_send_now is check-AND-CONSUME: it
         * calls hu_proactive_throttle_record_send, which appends to the send
         * ring and counts toward the SAME per-contact daily/weekly caps real
         * proactive check-ins use. Charging the live ledger here would be
         * wrong twice over — in SHADOW it would silently suppress real sends
         * (a mode that must have no production effect), and in LIVE it would
         * double-charge, because gate_and_send records the send itself and
         * would then trip its own send-cap (observed during TDD:
         * "proactive check-in skipped: send-cap" on the very message this
         * watcher had just proposed). gate_and_send stays the single authority
         * that debits the real ledger.
         *
         * One copy per TICK, on the heap: the struct is 121 KB (a 1024-entry
         * send ring), far too large for a stack frame inside the per-contact
         * loop of a daemon thread. Sharing it across the tick's candidates
         * also matches live semantics — a second candidate sees the first
         * one's provisional charge. It is POD and record_send never
         * allocates, so the copy yields an identical verdict.
         *
         * If the copy cannot be made we skip the scan entirely rather than
         * fall back to the live ledger: silently charging production budget
         * is exactly the failure this guards against. */
        hu_proactive_throttle_t *predicate_throttle = NULL;
        bool shadow_alloc_failed = false;
        if (throttle) {
            if (agent->alloc && agent->alloc->alloc)
                predicate_throttle = agent->alloc->alloc(agent->alloc->ctx, sizeof(*throttle));
            if (predicate_throttle)
                memcpy(predicate_throttle, throttle, sizeof(*throttle));
            else
                shadow_alloc_failed = true;
        }

        if (shadow_alloc_failed) {
            hu_log_warn("follow_up_watcher", agent->observer,
                        "follow-up scan skipped this tick: could not allocate the %zu-byte "
                        "shadow throttle ledger; refusing to spend the live proactive budget "
                        "on a shadow-mode predicate",
                        sizeof(*throttle));
            *last_poll_unix_inout = now_unix;
            *watermark_inout = now_unix;
            return HU_OK;
        }

        /* `on` sends only when EVERY prerequisite is present. A missing
         * gov_budget is not a soft default here: the governor skips the gate
         * on NULL (init_proposer.c:168) and send_and_record only debits
         * `if (gov_budget)` (daemon_proactive.c:1130), so sending without it
         * would be unlimited by and invisible to the shared daily budget. */
        char blockers[256];
        bool blocked = follow_up_on_blockers(blockers, sizeof(blockers), g_text_fn != NULL,
                                             gov_budget != NULL, ar_cfg != NULL);

        follow_up_tick_ctx_t tc = {
            .mode = mode,
            .agent = agent,
            .channel = NULL,
            .ch_name = NULL,
            .throttle = throttle,
            .predicate_throttle = predicate_throttle,
            .gov_budget = gov_budget,
            .ar_cfg = ar_cfg,
            .now_unix = now_unix,
            .ymd = follow_up_utc_ymd(now_unix),
            .tz_offset_s = tz_offset_s,
            .can_send = (mode == HU_GATE_LIVE) && !blocked,
            .blockers = blockers,
        };

        for (size_t ci = 0; ci < channel_count; ci++) {
            hu_channel_t *ch = channels[ci].channel;
            if (!ch || !ch->vtable || !ch->vtable->name)
                continue;
            const char *ch_name = ch->vtable->name(ch->ctx);
            /* chat.db is the only inbound-unreplied oracle we have. */
            if (!ch_name || strcmp(ch_name, "imessage") != 0)
                continue;

            tc.channel = ch;
            tc.ch_name = ch_name;
            for (size_t pi = 0; pi < agent->persona->contacts_count; pi++)
                follow_up_scan_contact(&tc, &agent->persona->contacts[pi]);
        }

        /* Free whenever we allocated. An allocator that hands out memory but
         * has no free is a broken contract, not a reason to leak 121 KB every
         * tick — say so loudly rather than swallowing it. */
        if (predicate_throttle) {
            if (agent->alloc && agent->alloc->free)
                agent->alloc->free(agent->alloc->ctx, predicate_throttle, sizeof(*throttle));
            else
                hu_log_warn("follow_up_watcher", agent->observer,
                            "allocator contract violation: alloc() succeeded but free() is NULL; "
                            "leaking the %zu-byte shadow throttle ledger this tick",
                            sizeof(*throttle));
        }
    }

    *last_poll_unix_inout = now_unix;
    *watermark_inout = now_unix;

    return HU_OK;
}
