/*
 * src/daemon/daemon_prospective.c — prospective memory v2 in the reactive
 * path. Contract in include/human/daemon/prospective.h.
 *
 * HU_PROSPECTIVE activation gated on the spec §3 promotion measurement
 * (scripts/pm_bench_local.py PASS + 30 SHADOW would-fires spot-checked blind
 * with precision >= 0.8, scripts/prospective_spot_check.py): do not flip to
 * default-ON without it.
 */
#include "human/daemon/prospective.h"

#include "human/core/string.h"
#include "human/memory/prospective_policy.h"
#include <string.h>

size_t hu_daemon_prospective_history_render(const hu_channel_history_entry_t *entries, size_t n,
                                            char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!entries || n == 0)
        return 0;
    size_t first = n > HU_PROSPECTIVE_HISTORY_TURNS ? n - HU_PROSPECTIVE_HISTORY_TURNS : 0;
    size_t need = 0;
    size_t start = n;
    while (start > first) { /* keep the newest lines that fit */
        const hu_channel_history_entry_t *e = &entries[start - 1];
        size_t line = strnlen(e->text, sizeof(e->text)) + (e->from_me ? 4 : 6) + 1;
        if (need + line >= cap)
            break;
        need += line;
        start--;
    }
    size_t pos = 0;
    for (size_t i = start; i < n; i++)
        pos =
            hu_buf_appendf(buf, cap, pos, "%s: %.*s\n", entries[i].from_me ? "me" : "them",
                           (int)strnlen(entries[i].text, sizeof(entries[i].text)), entries[i].text);
    return pos;
}

#ifdef HU_ENABLE_SQLITE

#include "human/agent.h"
#include "human/core/log.h"
#include "human/daemon/share_queue.h"
#include "human/memory.h"
#include "human/memory/prospective.h"
#include "human/providers/chat_oneshot.h"
#include <stdatomic.h>
#include <time.h>

/* SHADOW uptake: the would-fire actions of the last reactive turn, checked
 * against the reply actually delivered to the same contact. One slot — the
 * daemon runs one reactive turn at a time. */
static struct {
    char contact[128];
    char actions[HU_PROSPECTIVE_RENDER_CAP][256];
    size_t n;
} s_pm_shadow;

/* LIVE ownership of the surfaced set (task-7 ruling 2). A legacy keyword row
 * with contact_id NULL is listed by EVERY contact's keyword read
 * (hu_prospective_repo_list), so without an owner a row contact A's pass
 * surfaced could be settled by contact B: by B's pass's reclaim step, or by
 * B's delivery marking it used against B's reply. The owner is the contact
 * whose LIVE pass last surfaced something. Only the owner's delivery settles
 * the surfaced set; a LIVE pass for anyone else first settles it as the
 * owner's undelivered attempt (reply NULL: attempts+1). One slot, keyed by
 * the db it was claimed on: the daemon serializes reactive turns, and a
 * restart simply drops the slot (the rows are then reclaimed by the next
 * keyword pass, as an attempt that did not land). */
static struct {
    const sqlite3 *db;
    char contact[128];
} s_pm_live_owner;

static atomic_bool s_pm_banner_once = false;

static bool pm_owner_is(const sqlite3 *db, const char *contact, size_t contact_len) {
    return s_pm_live_owner.db == db && s_pm_live_owner.contact[0] &&
           strlen(s_pm_live_owner.contact) == contact_len &&
           memcmp(s_pm_live_owner.contact, contact, contact_len) == 0;
}

static void pm_owner_clear(void) {
    s_pm_live_owner.db = NULL;
    s_pm_live_owner.contact[0] = '\0';
}

/* Before a LIVE pass for `turn->contact`: a different owner's surfaced set
 * did not land (its delivery never came), so settle it under the owner. */
static void pm_owner_settle_foreign(hu_allocator_t *alloc, sqlite3 *db,
                                    const hu_prospective_turn_t *turn) {
    if (!s_pm_live_owner.contact[0] || pm_owner_is(db, turn->contact, turn->contact_len))
        return;
    if (s_pm_live_owner.db == db)
        (void)hu_prospective_v2_after_delivery(
            alloc, db, HU_PM_CUE_KEYWORD, s_pm_live_owner.contact, strlen(s_pm_live_owner.contact),
            NULL, 0, turn->now, NULL);
    pm_owner_clear();
}

static void pm_owner_claim(const sqlite3 *db, const hu_prospective_turn_t *turn,
                           const hu_prospective_counts_t *c) {
    /* surfaced this pass = fire_action_count attempted minus write failures */
    if (c->fire_action_count <= c->write_err ||
        turn->contact_len >= sizeof(s_pm_live_owner.contact))
        return;
    s_pm_live_owner.db = db;
    memcpy(s_pm_live_owner.contact, turn->contact, turn->contact_len);
    s_pm_live_owner.contact[turn->contact_len] = '\0';
}

hu_error_t hu_daemon_prospective_provider_judge(void *ctx, hu_allocator_t *alloc,
                                                const char *system, size_t system_len,
                                                const char *user, size_t user_len, char **out,
                                                size_t *out_len) {
    const hu_daemon_prospective_judge_ctx_t *jc = (const hu_daemon_prospective_judge_ctx_t *)ctx;
    if (!jc || !jc->provider)
        return HU_ERR_INVALID_ARGUMENT;
    /* One word back. Thinking off: GLM on :8741 otherwise reasons out loud
     * and spends the whole budget before the verdict. */
    const hu_chat_oneshot_opts_t opts = {
        .temperature = 0.0, .max_tokens = 16, .json_object = false};
    return hu_provider_chat_oneshot(alloc, jc->provider, jc->model, jc->model ? jc->model_len : 0,
                                    system, system_len, user, user_len, &opts, out, out_len);
}

void hu_daemon_prospective_log_counts(const char *tag, const hu_prospective_counts_t *c) {
    if (!tag || !c || c->candidates + c->expired + c->capped == 0)
        return;
    hu_log_info("prospective", NULL,
                "prospective %s: candidates=%zu fire=%zu resolved=%zu cancel=%zu not_now=%zu "
                "parse_fail=%zu judge_err=%zu expired=%zu capped=%zu write_err=%zu",
                tag, c->candidates, c->fire, c->resolved, c->cancel, c->not_now, c->parse_fail,
                c->judge_err, c->expired, c->capped, c->write_err);
    for (size_t i = 0; i < c->item_count; i++)
        hu_log_info("prospective", NULL, "prospective %s item: id=%lld verdict=%s", tag,
                    (long long)c->items[i].id,
                    c->items[i].judge_ok ? hu_prospective_verdict_str(c->items[i].verdict)
                                         : "judge_err");
}

static void pm_shadow_remember(const hu_prospective_turn_t *turn,
                               const hu_prospective_counts_t *c) {
    s_pm_shadow.n = 0;
    if (c->fire_action_count == 0 || turn->contact_len >= sizeof(s_pm_shadow.contact))
        return;
    memcpy(s_pm_shadow.contact, turn->contact, turn->contact_len);
    s_pm_shadow.contact[turn->contact_len] = '\0';
    for (size_t i = 0; i < c->fire_action_count; i++)
        memcpy(s_pm_shadow.actions[i], c->fire_actions[i], sizeof(s_pm_shadow.actions[i]));
    s_pm_shadow.n = c->fire_action_count;
}

char *hu_daemon_prospective_directive(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                      const hu_prospective_turn_t *turn,
                                      const hu_prospective_judge_t *judge, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || !db || !turn || !out_len || !turn->contact || turn->contact_len == 0)
        return NULL;
    hu_prospective_counts_t c;
    if (mode == HU_GATE_LIVE) {
        pm_owner_settle_foreign(alloc, db, turn);
        char *d = NULL;
        size_t dl = 0;
        hu_error_t err =
            hu_prospective_v2_run(alloc, db, HU_PM_CUE_KEYWORD, turn, judge, true, &c, &d, &dl);
        pm_owner_claim(db, turn, &c); /* even on error: a surfaced row needs an owner */
        if (err != HU_OK)
            return NULL; /* fail toward silence: no directive, nothing retired */
        hu_daemon_prospective_log_counts("live", &c);
        *out_len = dl;
        return d;
    }
    if (mode == HU_GATE_SHADOW && hu_prospective_v2_run(alloc, db, HU_PM_CUE_KEYWORD, turn, judge,
                                                        false, &c, NULL, NULL) == HU_OK) {
        hu_daemon_prospective_log_counts("shadow", &c);
        pm_shadow_remember(turn, &c);
    }
    /* OFF and SHADOW: today's directive, unchanged (fire on match, fired=1). */
    return hu_prospective_directive_build(alloc, db, turn->inbound, turn->inbound_len,
                                          turn->contact, turn->contact_len, turn->now, out_len);
}

void hu_daemon_prospective_on_delivered(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                        const char *contact, size_t contact_len, const char *reply,
                                        size_t reply_len, int64_t now) {
    if (!alloc || !db || !contact || contact_len == 0)
        return;
    if (mode == HU_GATE_LIVE) {
        if (!pm_owner_is(db, contact, contact_len))
            return; /* this contact surfaced nothing outstanding: settle nothing */
        pm_owner_clear();
        hu_prospective_delivery_counts_t dc;
        if (hu_prospective_v2_after_delivery(alloc, db, HU_PM_CUE_KEYWORD, contact, contact_len,
                                             reply, reply_len, now, &dc) == HU_OK &&
            dc.surfaced > 0)
            hu_log_info("prospective", NULL,
                        "prospective live delivered: surfaced=%zu used=%zu ignored=%zu "
                        "expired=%zu",
                        dc.surfaced, dc.used, dc.ignored, dc.expired);
        return;
    }
    if (mode != HU_GATE_SHADOW || s_pm_shadow.n == 0 ||
        strlen(s_pm_shadow.contact) != contact_len ||
        memcmp(s_pm_shadow.contact, contact, contact_len) != 0)
        return;
    size_t used = 0;
    for (size_t i = 0; i < s_pm_shadow.n; i++)
        if (hu_prospective_reply_uses_action(s_pm_shadow.actions[i], reply, reply_len))
            used++;
    hu_log_info("prospective", NULL, "prospective shadow uptake: would_fire=%zu used=%zu",
                s_pm_shadow.n, used);
    s_pm_shadow.n = 0;
}

char *hu_daemon_prospective_reactive(hu_allocator_t *alloc, struct hu_agent *agent, sqlite3 *db,
                                     const char *contact, size_t contact_len, const char *text,
                                     size_t text_len, const hu_channel_history_entry_t *history,
                                     size_t history_n, bool is_group, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || !db || !contact || contact_len == 0 || !text || text_len == 0 || !out_len)
        return NULL;
    hu_gate_mode_t mode = hu_prospective_gate_mode();
    hu_log_info_once(&s_pm_banner_once, "prospective", NULL, "%s",
                     hu_prospective_gate_banner(mode, false));
    int64_t now = (int64_t)time(NULL);
    if (mode == HU_GATE_OFF) /* byte-identical to the pre-v2 call sites */
        return hu_prospective_directive_build(alloc, db, text, text_len, contact, contact_len, now,
                                              out_len);
    char hist[6144];
    hu_prospective_turn_t turn;
    memset(&turn, 0, sizeof(turn));
    turn.contact = contact;
    turn.contact_len = contact_len;
    turn.inbound = text;
    turn.inbound_len = text_len;
    turn.history = hist;
    turn.history_len = hu_daemon_prospective_history_render(history, history_n, hist, sizeof(hist));
    turn.is_group = is_group;
    turn.is_self = agent && hu_share_is_owner(agent->persona, contact, contact_len);
    turn.now = now;
    turn.day_start = hu_prospective_local_day_start(now);
    hu_daemon_prospective_judge_ctx_t jc = {
        .provider = agent ? &agent->provider : NULL,
        .model = agent ? agent->model_name : NULL,
        .model_len = agent ? agent->model_name_len : 0,
    };
    hu_prospective_judge_t judge = {.fn = hu_daemon_prospective_provider_judge, .ctx = &jc};
    return hu_daemon_prospective_directive(alloc, db, mode, &turn, &judge, out_len);
}

void hu_daemon_prospective_delivered(struct hu_agent *agent, const char *target, size_t target_len,
                                     const char *text, size_t text_len) {
    if (!agent || !agent->memory || !agent->alloc || !target || target_len == 0)
        return;
    hu_gate_mode_t mode = hu_prospective_gate_mode();
    if (mode == HU_GATE_OFF)
        return;
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (db)
        hu_daemon_prospective_on_delivered(agent->alloc, db, mode, target, target_len, text,
                                           text_len, (int64_t)time(NULL));
}

#endif /* HU_ENABLE_SQLITE */
