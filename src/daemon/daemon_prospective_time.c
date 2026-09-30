/*
 * src/daemon/daemon_prospective_time.c — time-cued follow-ups for the
 * proactive tick. See include/human/daemon/prospective_time.h.
 *
 * Both producers are the hu_service_run bodies as of b622a96d4, moved
 * verbatim apart from the out-parameters (commitment_ctx / commitment_ids /
 * due_fu_buf / due_followup_id_listed were locals there). HU_PROSPECTIVE_TIME
 * (task 9) chooses between them and the v2 per-contact due set.
 *
 * HU_PROSPECTIVE_TIME activation gated on its own 7-day SHADOW read after
 * HU_PROSPECTIVE passes promotion (spec §4.4, §6 step 5): do not flip to
 * default-ON without it.
 */
#include "human/daemon/prospective_time.h"

#include "human/agent.h"
#include "human/channel.h"
#include "human/memory/superhuman.h"

#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/prospective.h"
#include "human/daemon/share_queue.h"
#include "human/memory.h"
#include "human/memory/prospective_policy.h"
#include "human/memory/prospective_v2.h"
#include <stdatomic.h>

static atomic_bool s_pm_time_banner_once = false;

/* The gate, read once per call, with its one banner line per process
 * (silent-config-gated-subsystems: OFF says so too). */
static hu_gate_mode_t pm_time_mode(void) {
    hu_gate_mode_t mode = hu_prospective_time_gate_mode();
    hu_log_info_once(&s_pm_time_banner_once, "prospective", NULL, "%s",
                     hu_prospective_gate_banner(mode, true));
    return mode;
}

/* SHADOW runs on every proactive tick but judges a contact once per local
 * day, so its would-send numbers are per day, not per tick (LIVE needs no
 * such slot: the store's surfaced_at is its per-day cap). A hash collision
 * only skips a SHADOW pass, never changes what is sent. */
#define PM_TIME_SHADOW_SLOTS 64
static struct {
    uint64_t hash;
    int64_t day;
} s_pm_time_seen[PM_TIME_SHADOW_SLOTS];

static bool pm_time_first_today(const char *contact, int64_t day) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = contact; *p; p++) {
        h ^= (uint64_t)(unsigned char)*p;
        h *= 1099511628211ULL;
    }
    size_t slot = (size_t)(h % PM_TIME_SHADOW_SLOTS);
    if (s_pm_time_seen[slot].hash == h && s_pm_time_seen[slot].day == day)
        return false;
    s_pm_time_seen[slot].hash = h;
    s_pm_time_seen[slot].day = day;
    return true;
}

/* The send channel's recent history as "me: …" / "them: …" lines: the
 * proactive tick has no conversation loaded for the fire-time check. */
static size_t pm_time_history(hu_allocator_t *alloc, struct hu_channel *ch, const char *target,
                              size_t target_len, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!ch || !ch->vtable || !ch->vtable->load_conversation_history || !target || target_len == 0)
        return 0;
    hu_channel_history_entry_t *entries = NULL;
    size_t n = 0;
    size_t len = 0;
    if (ch->vtable->load_conversation_history(ch->ctx, alloc, target, target_len,
                                              HU_PROSPECTIVE_HISTORY_TURNS, &entries,
                                              &n) == HU_OK &&
        entries)
        len = hu_daemon_prospective_history_render(entries, n, buf, cap);
    if (entries)
        alloc->free(alloc->ctx, entries, n * sizeof(hu_channel_history_entry_t));
    return len;
}

/* One v2 time pass for the contact. live: apply, and copy the due line into
 * buf; shadow: read-only (apply=false writes nothing), logged, nothing
 * returned. Every read and settle is scoped to `contact_id` (time rows are
 * never contact-less), so one contact's pass never touches another's rows. */
static size_t pm_time_v2(hu_allocator_t *alloc, struct hu_agent *agent, struct hu_channel *ch,
                         const char *target, size_t target_len, const char *contact_id, int64_t now,
                         bool live, char *buf, size_t cap) {
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    int64_t day = hu_prospective_local_day_start(now);
    if (!db || (!live && !pm_time_first_today(contact_id, day)))
        return 0;
    char hist[6144];
    size_t clen = strlen(contact_id);
    hu_prospective_turn_t turn = {
        .contact = contact_id,
        .contact_len = clen,
        .history = hist,
        .history_len = pm_time_history(alloc, ch, target, target_len, hist, sizeof(hist)),
        .is_self = hu_share_is_owner(agent->persona, contact_id, clen),
        .now = now,
        .day_start = day,
    };
    hu_daemon_prospective_judge_ctx_t jc = {.provider = &agent->provider,
                                            .model = agent->model_name,
                                            .model_len = agent->model_name_len};
    hu_prospective_judge_t judge = {.fn = hu_daemon_prospective_provider_judge, .ctx = &jc};
    hu_prospective_counts_t c = {0};
    char *d = NULL;
    size_t dl = 0;
    hu_error_t err = hu_prospective_v2_run(alloc, db, HU_PM_CUE_TIME, &turn, &judge, live, &c,
                                           live ? &d : NULL, live ? &dl : NULL);
    hu_daemon_prospective_log_counts(live ? "time live" : "time shadow", &c); /* on error too */
    size_t n = 0;
    if (err == HU_OK && d && dl > 0 && dl < cap) {
        memcpy(buf, d, dl + 1);
        n = dl;
    } else if (d) {
        /* Surfaced but not shown (cannot fit): the next pass settles it as
         * an attempt that did not land. Fail toward silence. */
        hu_log_warn("prospective", NULL, "prospective time live: due set of %zu B not shown", dl);
    }
    if (d)
        alloc->free(alloc->ctx, d, dl + 1);
    return n;
}
#endif /* HU_ENABLE_SQLITE */

void hu_daemon_prospective_commitment_ctx(hu_allocator_t *alloc, struct hu_agent *agent,
                                          const char *contact_id, int64_t now, char **ctx_out,
                                          size_t *ctx_len_out, int64_t ids_out[3],
                                          size_t *ids_count_out) {
    if (ctx_out)
        *ctx_out = NULL;
    if (ctx_len_out)
        *ctx_len_out = 0;
    if (ids_count_out)
        *ids_count_out = 0;
    if (!alloc || !agent || !agent->memory || !contact_id || !ctx_out || !ctx_len_out || !ids_out ||
        !ids_count_out)
        return;
#ifdef HU_ENABLE_SQLITE
    /* LIVE: commitments are mirrored into the typed store and reach the
     * proposer through the per-contact due set — never twice. */
    if (pm_time_mode() == HU_GATE_LIVE)
        return;
#endif
    hu_superhuman_commitment_t *due = NULL;
    size_t due_count = 0;
    if (hu_superhuman_commitment_list_due(agent->memory, alloc, now, 3, &due, &due_count) !=
            HU_OK ||
        !due || due_count == 0)
        return;
    size_t cid_len = strlen(contact_id);
    char ctx_buf[1024];
    size_t ctx_pos = 0;
    for (size_t di = 0; di < due_count && ctx_pos < sizeof(ctx_buf) - 200; di++) {
        if (cid_len != strlen(due[di].contact_id) ||
            memcmp(due[di].contact_id, contact_id, cid_len) != 0)
            continue;
        int n = snprintf(ctx_buf + ctx_pos, sizeof(ctx_buf) - ctx_pos,
                         "COMMITMENT FOLLOW-UP: %s was due. Ask if it happened: "
                         "'hey did you ever %s?'\n",
                         due[di].description, due[di].description);
        if (n > 0 && ctx_pos + (size_t)n < sizeof(ctx_buf)) {
            ctx_pos += (size_t)n;
            if (*ids_count_out < 3)
                ids_out[(*ids_count_out)++] = due[di].id;
        }
    }
    if (ctx_pos > 0) {
        char *c = (char *)alloc->alloc(alloc->ctx, ctx_pos + 1);
        if (c) {
            memcpy(c, ctx_buf, ctx_pos);
            c[ctx_pos] = '\0';
            *ctx_out = c;
            *ctx_len_out = ctx_pos;
        }
    }
    hu_superhuman_commitment_free(alloc, due, due_count);
}

/* Items are listed, not marked sent: mark-sent stays tied to an actual send
 * (the F31 path at the send site), so an unsent item correctly reappears. */
static size_t pm_legacy_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                      const char *contact_id, int64_t now, char *buf, size_t cap,
                                      int64_t *listed_id) {
    hu_delayed_followup_t *due_arr = NULL;
    size_t due_n = 0;
    if (hu_superhuman_delayed_followup_list_due(agent->memory, alloc, now, &due_arr, &due_n) !=
            HU_OK ||
        !due_arr || due_n == 0)
        return 0;
    size_t pos = 0;
    size_t listed = 0;
    for (size_t fi = 0; fi < due_n && listed < 1; fi++) {
        if (strcmp(due_arr[fi].contact_id, contact_id) != 0)
            continue;
        if (listed_id)
            *listed_id = due_arr[fi].id;
        int w = snprintf(buf + pos, cap - pos, "- %s (due %llds ago)\n", due_arr[fi].topic,
                         (long long)(now - due_arr[fi].scheduled_at));
        if (w <= 0 || (size_t)w >= cap - pos)
            break;
        pos += (size_t)w;
        listed++;
    }
    hu_superhuman_delayed_followup_free(alloc, due_arr, due_n);
    return pos;
}

size_t hu_daemon_prospective_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                           struct hu_channel *ch, const char *target,
                                           size_t target_len, const char *contact_id, int64_t now,
                                           char *buf, size_t cap, int64_t *listed_id) {
    if (buf && cap > 0)
        buf[0] = '\0';
    if (!alloc || !agent || !agent->memory || !contact_id || !buf || cap == 0)
        return 0;
#ifdef HU_ENABLE_SQLITE
    hu_gate_mode_t mode = pm_time_mode();
    if (mode == HU_GATE_LIVE) /* *listed_id untouched: the legacy mark-sent stays out */
        return pm_time_v2(alloc, agent, ch, target, target_len, contact_id, now, true, buf, cap);
    if (mode == HU_GATE_SHADOW)
        (void)pm_time_v2(alloc, agent, ch, target, target_len, contact_id, now, false, NULL, 0);
#else
    (void)ch;
    (void)target;
    (void)target_len;
#endif
    return pm_legacy_due_followups(alloc, agent, contact_id, now, buf, cap, listed_id);
}

void hu_daemon_prospective_time_after_send(struct hu_agent *agent, const char *contact_id,
                                           const char *text, size_t text_len, int64_t now) {
#ifdef HU_ENABLE_SQLITE
    if (!agent || !agent->memory || !agent->alloc || !contact_id || !contact_id[0] ||
        pm_time_mode() != HU_GATE_LIVE)
        return;
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return;
    hu_prospective_delivery_counts_t dc;
    hu_error_t err = hu_prospective_v2_after_delivery(agent->alloc, db, HU_PM_CUE_TIME, contact_id,
                                                      strlen(contact_id), text, text_len, now, &dc);
    if (err != HU_OK)
        hu_log_warn("prospective", NULL, "prospective time live delivered: settle failed: %s",
                    hu_error_string(err));
    else if (dc.surfaced > 0)
        hu_log_info("prospective", NULL,
                    "prospective time live delivered: surfaced=%zu used=%zu ignored=%zu "
                    "expired=%zu",
                    dc.surfaced, dc.used, dc.ignored, dc.expired);
#else
    (void)agent;
    (void)contact_id;
    (void)text;
    (void)text_len;
    (void)now;
#endif
}
