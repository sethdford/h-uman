/*
 * src/memory/prospective_v2.c — Filter -> Decide over the typed intention
 * store. Contract in include/human/memory/prospective_v2.h; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.
 * All SQL is in src/memory/repos/prospective_repo_sqlite.c.
 */
#include "human/memory/prospective_v2.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/string.h"
#include <string.h>

#define PM_USER_CAP 6144

static bool pm_same_intention(const hu_prospective_item_t *a, const hu_prospective_item_t *b) {
    return strcmp(a->action, b->action) == 0 && strcmp(a->contact_id, b->contact_id) == 0;
}

static void pm_mark_handled(const hu_prospective_item_t *items, size_t n, bool *handled, size_t i) {
    for (size_t k = 0; k < n; k++)
        if (!handled[k] && pm_same_intention(&items[k], &items[i]))
            handled[k] = true;
}

/* Retire an intention; a time intention also retires its ledger twins, but
 * only when the primary transition actually changed a row. `changed` (may
 * be NULL) receives hu_prospective_repo_transition's row count, and the
 * transition's own hu_error_t is returned so a real backend failure is
 * never silently swallowed. */
static hu_error_t pm_retire(sqlite3 *db, const hu_prospective_item_t *it,
                            hu_prospective_status_t to, hu_prospective_outcome_t outcome,
                            int attempts, int64_t now, int *changed) {
    int ch = 0;
    hu_error_t err = hu_prospective_repo_transition(db, it, to, outcome, attempts, now, &ch);
    if (changed)
        *changed = ch;
    if (err == HU_OK && ch > 0 && it->cue_kind == HU_PM_CUE_TIME && to != HU_PM_PENDING)
        (void)hu_prospective_repo_sync_source(db, it, to, now);
    return err;
}

static hu_prospective_verdict_t pm_judge(hu_allocator_t *alloc, const hu_prospective_judge_t *judge,
                                         const hu_prospective_turn_t *turn,
                                         const hu_prospective_item_t *it, bool *judge_ok) {
    *judge_ok = false;
    if (!judge || !judge->fn)
        return HU_PM_VERDICT_PARSE_FAIL;
    bool is_time = it->cue_kind == HU_PM_CUE_TIME;
    char user[PM_USER_CAP];
    size_t ul = hu_prospective_judge_user(user, sizeof(user), turn->history, turn->history_len,
                                          it->action, is_time ? NULL : it->trigger_value,
                                          it->cue_kind, is_time ? turn->now - it->due_at : 0);
    if (ul == 0)
        return HU_PM_VERDICT_PARSE_FAIL;
    size_t sl = 0;
    const char *sys = hu_prospective_judge_system(&sl);
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = judge->fn(judge->ctx, alloc, sys, sl, user, ul, &raw, &raw_len);
    hu_prospective_verdict_t v = HU_PM_VERDICT_PARSE_FAIL;
    if (err == HU_OK) {
        *judge_ok = true;
        v = hu_prospective_parse_verdict(raw, raw_len);
    }
    if (raw)
        alloc->free(alloc->ctx, raw, raw_len + 1);
    return v;
}

static bool pm_keyword_cued(const hu_prospective_turn_t *turn, const hu_prospective_item_t *it) {
    return it->trigger_value[0] && turn->inbound && turn->inbound_len > 0 &&
           hu_str_contains_word_ci_n(turn->inbound, turn->inbound_len, it->trigger_value);
}

static void pm_count_verdict(hu_prospective_counts_t *c, const hu_prospective_item_t *it, bool ok,
                             hu_prospective_verdict_t v) {
    if (c->item_count < HU_PROSPECTIVE_JUDGE_CAP) {
        c->items[c->item_count].id = it->id;
        c->items[c->item_count].verdict = v;
        c->items[c->item_count].judge_ok = ok;
        c->item_count++;
    }
}

/* Surface exactly what the directive shows (task 7 review, fix round 1):
 *   1. render the fired intentions. hu_prospective_render keeps a PREFIX of
 *      them, and two long actions can overflow the 1024-byte buffer, so
 *      `shown` may be fewer than fire_n; the rest are never touched and stay
 *      pending with their attempts intact;
 *   2. allocate the directive BEFORE any write, so an allocation failure
 *      leaves every row pending (nothing surfaced, nothing shown);
 *   3. write-before-show: mark each shown intention surfaced and keep in the
 *      text only those whose write changed a row. A failed or no-op write
 *      (counted write_err) is dropped from the text and stays pending;
 *   4. when a write failed, re-render the survivors (a subset of a prefix
 *      that fit, so it fits) and shrink the buffer to their exact size. If
 *      that shrink fails, the survivors are put back to pending (attempts
 *      unchanged; surfaced_at keeps the stamp just written, which only the
 *      time cue's per-day cap reads) and HU_ERR_OUT_OF_MEMORY is returned.
 * With `directive` NULL nothing is shown, so nothing is surfaced. */
static hu_error_t pm_surface(hu_allocator_t *alloc, sqlite3 *db, hu_prospective_cue_kind_t kind,
                             const hu_prospective_item_t *const *fire, size_t fire_n, int64_t now,
                             hu_prospective_counts_t *counts, char **directive,
                             size_t *directive_len) {
    if (!directive || fire_n == 0)
        return HU_OK;
    const hu_prospective_render_style_t style =
        kind == HU_PM_CUE_TIME ? HU_PM_RENDER_DUE_LIST : HU_PM_RENDER_SOFT;
    const char *acts[HU_PROSPECTIVE_RENDER_CAP];
    const char *cues[HU_PROSPECTIVE_RENDER_CAP];
    for (size_t k = 0; k < fire_n; k++) {
        acts[k] = fire[k]->action;
        cues[k] = fire[k]->trigger_value;
    }
    char buf[1024];
    size_t blen = 0;
    size_t shown = hu_prospective_render(style, acts, cues, fire_n, buf, sizeof(buf), &blen);
    if (shown == 0)
        return HU_OK;
    char *d = (char *)alloc->alloc(alloc->ctx, blen + 1);
    if (!d)
        return HU_ERR_OUT_OF_MEMORY; /* before any write: every row stays pending */
    const hu_prospective_item_t *ok[HU_PROSPECTIVE_RENDER_CAP];
    size_t ok_n = 0;
    for (size_t k = 0; k < shown; k++) {
        int changed = 0;
        hu_error_t terr = hu_prospective_repo_transition(
            db, fire[k], HU_PM_SURFACED, HU_PM_OUTCOME_NONE, fire[k]->attempts, now, &changed);
        if (terr != HU_OK || changed <= 0) {
            counts->write_err++;
            continue;
        }
        acts[ok_n] = fire[k]->action;
        cues[ok_n] = fire[k]->trigger_value;
        ok[ok_n++] = fire[k];
    }
    if (ok_n == 0) {
        alloc->free(alloc->ctx, d, blen + 1);
        return HU_OK;
    }
    size_t dlen = blen;
    if (ok_n < shown) {
        size_t sub = 0;
        size_t r = hu_prospective_render(style, acts, cues, ok_n, buf, sizeof(buf), &sub);
        char *nd = r == ok_n ? (char *)alloc->realloc(alloc->ctx, d, blen + 1, sub + 1) : NULL;
        if (!nd) {
            alloc->free(alloc->ctx, d, blen + 1);
            for (size_t k = 0; k < ok_n; k++)
                (void)hu_prospective_repo_transition(db, ok[k], HU_PM_PENDING, HU_PM_OUTCOME_NONE,
                                                     ok[k]->attempts, now, NULL);
            return HU_ERR_OUT_OF_MEMORY;
        }
        d = nd;
        dlen = sub;
    }
    memcpy(d, buf, dlen + 1);
    *directive = d;
    if (directive_len)
        *directive_len = dlen;
    return HU_OK;
}

hu_error_t hu_prospective_v2_run(hu_allocator_t *alloc, sqlite3 *db, hu_prospective_cue_kind_t kind,
                                 const hu_prospective_turn_t *turn,
                                 const hu_prospective_judge_t *judge, bool apply,
                                 hu_prospective_counts_t *counts, char **directive,
                                 size_t *directive_len) {
    if (directive)
        *directive = NULL;
    if (directive_len)
        *directive_len = 0;
    if (!alloc || !db || !turn || !counts || !turn->contact || turn->contact_len == 0 ||
        (kind != HU_PM_CUE_KEYWORD && kind != HU_PM_CUE_TIME))
        return HU_ERR_INVALID_ARGUMENT;
    memset(counts, 0, sizeof(*counts));
    if (apply && !turn->is_group && !turn->is_self) {
        hu_error_t rerr = hu_prospective_v2_after_delivery(
            alloc, db, kind, turn->contact, turn->contact_len, NULL, 0, turn->now, NULL);
        if (rerr != HU_OK)
            return rerr;
    }
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    hu_error_t err = hu_prospective_repo_list(alloc, db, kind, HU_PM_PENDING, turn->contact,
                                              turn->contact_len, &items, &n);
    if (err != HU_OK || n == 0)
        return err;
    int64_t surfaced_today = 0;
    if (kind == HU_PM_CUE_TIME &&
        hu_prospective_repo_count_surfaced_since(db, kind, turn->contact, turn->contact_len,
                                                 turn->day_start, &surfaced_today) != HU_OK)
        surfaced_today = 1; /* cannot prove the day's slot is free: stay silent */
    bool *handled = (bool *)alloc->alloc(alloc->ctx, n * sizeof(bool));
    if (!handled) {
        hu_prospective_repo_free(alloc, items, n);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(handled, 0, n * sizeof(bool));
    const hu_prospective_item_t *fire[HU_PROSPECTIVE_RENDER_CAP];
    size_t fire_n = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_prospective_item_t *it = &items[i];
        if (handled[i])
            continue;
        hu_prospective_filter_facts_t f;
        memset(&f, 0, sizeof(f));
        f.cue_kind = kind;
        f.status = it->status;
        f.is_group = turn->is_group;
        f.is_self = turn->is_self;
        f.keyword_in_text = kind == HU_PM_CUE_KEYWORD && pm_keyword_cued(turn, it);
        f.due_at = it->due_at;
        f.expires_at = it->expires_at;
        f.now = turn->now;
        f.grace_s = HU_PROSPECTIVE_TIME_GRACE_S;
        f.surfaced_today = (size_t)surfaced_today + (kind == HU_PM_CUE_TIME ? fire_n : 0);
        hu_prospective_filter_t fr = hu_prospective_filter(&f);
        if (fr == HU_PM_FILTER_SKIP)
            continue;
        pm_mark_handled(items, n, handled, i);
        if (fr == HU_PM_FILTER_EXPIRE) {
            counts->expired++;
            if (apply)
                (void)pm_retire(db, it, HU_PM_EXPIRED, HU_PM_OUTCOME_NONE, it->attempts, turn->now,
                                NULL);
            continue;
        }
        if (fr == HU_PM_FILTER_CAPPED) {
            counts->capped++;
            continue;
        }
        if (counts->candidates >= HU_PROSPECTIVE_JUDGE_CAP)
            continue; /* bounded model calls per turn; the rest stay pending */
        counts->candidates++;
        bool ok = false;
        hu_prospective_verdict_t v = pm_judge(alloc, judge, turn, it, &ok);
        pm_count_verdict(counts, it, ok, v);
        switch (hu_prospective_decide(ok, v)) {
        case HU_PM_ACT_SURFACE:
            counts->fire++;
            if (fire_n < HU_PROSPECTIVE_RENDER_CAP)
                fire[fire_n++] = it;
            break;
        case HU_PM_ACT_MARK_DONE:
            counts->resolved++;
            if (apply)
                (void)pm_retire(db, it, HU_PM_DONE, HU_PM_OUTCOME_SUPPRESSED, it->attempts,
                                turn->now, NULL);
            break;
        case HU_PM_ACT_MARK_CANCELED:
            counts->cancel++;
            if (apply)
                (void)pm_retire(db, it, HU_PM_CANCELED, HU_PM_OUTCOME_SUPPRESSED, it->attempts,
                                turn->now, NULL);
            break;
        default:
            if (!ok)
                counts->judge_err++;
            else if (v == HU_PM_VERDICT_PARSE_FAIL)
                counts->parse_fail++;
            else
                counts->not_now++;
            break;
        }
    }
    for (size_t k = 0; k < fire_n; k++) {
        size_t al = strlen(fire[k]->action);
        if (al >= sizeof(counts->fire_actions[0]))
            al = sizeof(counts->fire_actions[0]) - 1;
        memcpy(counts->fire_actions[k], fire[k]->action, al);
        counts->fire_actions[k][al] = '\0';
    }
    counts->fire_action_count = fire_n;
    if (apply && fire_n > 0)
        err =
            pm_surface(alloc, db, kind, fire, fire_n, turn->now, counts, directive, directive_len);
    alloc->free(alloc->ctx, handled, n * sizeof(bool));
    hu_prospective_repo_free(alloc, items, n);
    return err;
}

hu_error_t hu_prospective_v2_after_delivery(hu_allocator_t *alloc, sqlite3 *db,
                                            hu_prospective_cue_kind_t kind, const char *contact,
                                            size_t contact_len, const char *reply, size_t reply_len,
                                            int64_t now, hu_prospective_delivery_counts_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!alloc || !db || !contact || contact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    hu_error_t err =
        hu_prospective_repo_list(alloc, db, kind, HU_PM_SURFACED, contact, contact_len, &items, &n);
    if (err != HU_OK || n == 0)
        return err;
    for (size_t i = 0; i < n; i++) {
        const hu_prospective_item_t *it = &items[i];
        bool dup = false;
        for (size_t k = 0; k < i && !dup; k++)
            dup = pm_same_intention(&items[k], it);
        if (dup)
            continue; /* the intention moved with its first row */
        bool used = hu_prospective_reply_uses_action(it->action, reply, reply_len);
        hu_prospective_status_t to =
            hu_prospective_after_delivery_status(used, it->attempts, HU_PROSPECTIVE_MAX_ATTEMPTS);
        int changed = 0;
        hu_error_t terr = pm_retire(db, it, to, used ? HU_PM_OUTCOME_USED : HU_PM_OUTCOME_IGNORED,
                                    used ? it->attempts : it->attempts + 1, now, &changed);
        if (terr != HU_OK) { /* a real backend error: propagate it, don't fabricate counts */
            hu_prospective_repo_free(alloc, items, n);
            return terr;
        }
        if (changed <= 0)
            continue; /* matched no row: nothing to count */
        if (out) {
            out->surfaced++;
            if (used)
                out->used++;
            else
                out->ignored++;
            if (to == HU_PM_EXPIRED)
                out->expired++;
        }
    }
    hu_prospective_repo_free(alloc, items, n);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
