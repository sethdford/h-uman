/*
 * proactive_throttle.c — Centralized throttle for proactive sends.
 *
 * See include/human/agent/proactive_throttle.h for full design rationale.
 *
 * Implementation notes:
 *   - Storage is flat arrays sized at compile time. Each table evicts via LRU
 *     when full. This keeps the memory bounded; agents that drift over the
 *     contact cap simply see older entries fall out (which fails-open on
 *     dedup but the per-contact daily cap still bounds the blast radius).
 *   - Time inputs are milliseconds-since-epoch so tests can supply
 *     deterministic timestamps without mocking the clock.
 */
#include "human/agent/proactive_throttle.h"

#include <stdio.h>
#include <string.h>

#define HU_THROTTLE_DEFAULT_DAILY_CAP  1u
#define HU_THROTTLE_DEFAULT_WEEKLY_CAP 3u
#define HU_THROTTLE_MS_PER_DAY         (24ULL * 3600ULL * 1000ULL)
#define HU_THROTTLE_MS_PER_WEEK        (7ULL * HU_THROTTLE_MS_PER_DAY)

void hu_proactive_throttle_init(hu_proactive_throttle_t *t, hu_allocator_t *alloc) {
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    t->alloc = alloc;
    t->daily_cap = HU_THROTTLE_DEFAULT_DAILY_CAP;
    t->weekly_cap = HU_THROTTLE_DEFAULT_WEEKLY_CAP;
}

void hu_proactive_throttle_reset(hu_proactive_throttle_t *t) {
    if (!t)
        return;
    hu_allocator_t *alloc = t->alloc;
    uint32_t daily = t->daily_cap;
    uint32_t weekly = t->weekly_cap;
    memset(t, 0, sizeof(*t));
    t->alloc = alloc;
    t->daily_cap = daily ? daily : HU_THROTTLE_DEFAULT_DAILY_CAP;
    t->weekly_cap = weekly ? weekly : HU_THROTTLE_DEFAULT_WEEKLY_CAP;
}

/* ── Channel rate limiter ─────────────────────────────────────────────── */

static uint64_t throttle_seq(hu_proactive_throttle_t *t) {
    /* Monotonic-ish counter for LRU comparisons. Wraps gracefully. */
    static uint64_t g_seq;
    (void)t;
    return ++g_seq;
}

static hu_proactive_throttle_channel_t *throttle_channel_slot(hu_proactive_throttle_t *t,
                                                              const char *name) {
    /* Look up existing. */
    for (size_t i = 0; i < t->channel_count; i++) {
        if (strncmp(t->channels[i].name, name, sizeof(t->channels[i].name) - 1) == 0)
            return &t->channels[i];
    }

    /* Allocate or evict. */
    size_t slot;
    if (t->channel_count < HU_PROACTIVE_THROTTLE_MAX_CHANNELS) {
        slot = t->channel_count++;
    } else {
        slot = 0;
        for (size_t i = 1; i < HU_PROACTIVE_THROTTLE_MAX_CHANNELS; i++) {
            if (t->channels[i].last_used_ms < t->channels[slot].last_used_ms)
                slot = i;
        }
    }
    hu_proactive_throttle_channel_t *c = &t->channels[slot];
    memset(c, 0, sizeof(*c));
    size_t n = strlen(name);
    if (n >= sizeof(c->name))
        n = sizeof(c->name) - 1;
    memcpy(c->name, name, n);
    c->name[n] = '\0';
    c->lim = hu_channel_rate_limit_default(name);
    return c;
}

bool hu_proactive_throttle_channel_try_consume(hu_proactive_throttle_t *t,
                                               const char *channel_name) {
    if (!t)
        return true; /* Fail-open: a NULL throttle means it isn't wired yet. */
    if (!channel_name || !channel_name[0])
        return true;

    hu_proactive_throttle_channel_t *c = throttle_channel_slot(t, channel_name);
    if (!c)
        return true;
    c->last_used_ms = throttle_seq(t);
    return hu_channel_rate_limiter_try_consume(&c->lim, 1);
}

/* ── Per-(feature, contact_id, ymd) dedup ─────────────────────────────── */

static void throttle_dedup_make_key(const char *feature, const char *contact_id, uint32_t ymd,
                                    char *out, size_t cap) {
    /* Compact, fixed-form key. We don't depend on contact_id length. */
    int n = snprintf(out, cap, "%.15s:%.63s:%u", feature ? feature : "",
                     contact_id ? contact_id : "", (unsigned)ymd);
    if (n < 0)
        out[0] = '\0';
}

bool hu_proactive_throttle_dedup_first_today(hu_proactive_throttle_t *t, const char *feature,
                                             const char *contact_id, uint32_t ymd) {
    if (!t || !feature || !contact_id || !*contact_id)
        return false;

    char key[sizeof(t->dedup[0].key)];
    throttle_dedup_make_key(feature, contact_id, ymd, key, sizeof(key));

    for (size_t i = 0; i < t->dedup_count; i++) {
        if (strncmp(t->dedup[i].key, key, sizeof(t->dedup[i].key)) == 0)
            return false; /* already delivered today */
    }

    /* Insert new entry — append or evict LRU. */
    size_t slot;
    if (t->dedup_count < HU_PROACTIVE_THROTTLE_MAX_CONTACTS) {
        slot = t->dedup_count++;
    } else {
        slot = 0;
        for (size_t i = 1; i < HU_PROACTIVE_THROTTLE_MAX_CONTACTS; i++) {
            if (t->dedup[i].last_used_ms < t->dedup[slot].last_used_ms)
                slot = i;
        }
    }
    hu_proactive_throttle_dedup_t *d = &t->dedup[slot];
    memset(d, 0, sizeof(*d));
    size_t kl = strlen(key);
    if (kl >= sizeof(d->key))
        kl = sizeof(d->key) - 1;
    memcpy(d->key, key, kl);
    d->key[kl] = '\0';
    d->ymd = ymd;
    d->last_used_ms = throttle_seq(t);
    return true;
}

/* ── Per-contact daily/weekly cap ─────────────────────────────────────── */

uint32_t hu_proactive_throttle_count_in_window(const hu_proactive_throttle_t *t,
                                               const char *contact_id, uint64_t now_ms,
                                               uint64_t window_ms) {
    if (!t || !contact_id || !*contact_id)
        return 0;
    uint32_t count = 0;
    for (size_t i = 0; i < t->send_count; i++) {
        const hu_proactive_throttle_send_t *s = &t->sends[i];
        if (s->contact_id[0] == '\0')
            continue;
        if (strncmp(s->contact_id, contact_id, sizeof(s->contact_id)) != 0)
            continue;
        if (now_ms >= s->sent_at_ms && (now_ms - s->sent_at_ms) < window_ms)
            count++;
    }
    return count;
}

bool hu_proactive_throttle_record_send(hu_proactive_throttle_t *t, const char *contact_id,
                                       const char *feature, uint64_t now_ms) {
    if (!t || !contact_id || !*contact_id)
        return false;

    uint32_t daily =
        hu_proactive_throttle_count_in_window(t, contact_id, now_ms, HU_THROTTLE_MS_PER_DAY);
    if (t->daily_cap > 0 && daily >= t->daily_cap)
        return false;
    uint32_t weekly =
        hu_proactive_throttle_count_in_window(t, contact_id, now_ms, HU_THROTTLE_MS_PER_WEEK);
    if (t->weekly_cap > 0 && weekly >= t->weekly_cap)
        return false;

    /* Append to ring. */
    size_t slot = t->send_head;
    hu_proactive_throttle_send_t *s = &t->sends[slot];
    memset(s, 0, sizeof(*s));
    size_t cl = strlen(contact_id);
    if (cl >= sizeof(s->contact_id))
        cl = sizeof(s->contact_id) - 1;
    memcpy(s->contact_id, contact_id, cl);
    s->contact_id[cl] = '\0';
    if (feature) {
        size_t fl = strlen(feature);
        if (fl >= sizeof(s->feature))
            fl = sizeof(s->feature) - 1;
        memcpy(s->feature, feature, fl);
        s->feature[fl] = '\0';
    }
    s->sent_at_ms = now_ms;
    t->send_head = (slot + 1) % HU_PROACTIVE_THROTTLE_MAX_SENDS;
    if (t->send_count < HU_PROACTIVE_THROTTLE_MAX_SENDS)
        t->send_count++;
    return true;
}
