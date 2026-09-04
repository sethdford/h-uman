/* Reactive reply budget — see include/human/daemon/send_budget.h. */
#include "human/daemon/send_budget.h"

#include <string.h>

static size_t key_len_capped(const char *contact, size_t contact_len) {
    if (!contact || !contact[0] || contact_len == 0)
        return 0;
    return contact_len < HU_SEND_BUDGET_KEY_CAP - 1 ? contact_len : HU_SEND_BUDGET_KEY_CAP - 1;
}

void hu_send_budget_init(hu_send_budget_t *b, uint32_t per_contact_hourly, uint32_t global_hourly) {
    if (!b)
        return;
    memset(b, 0, sizeof(*b));
    b->per_contact_hourly = per_contact_hourly;
    b->global_hourly = global_hourly;
}

/* Count ring entries inside the window. Entries are 0 when empty. */
static uint32_t count_in_window(const int64_t *ring, size_t n, int64_t now) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (ring[i] > 0 && now - ring[i] < HU_SEND_BUDGET_WINDOW_SEC)
            c++;
    }
    return c;
}

static const hu_send_budget_contact_t *find_contact(const hu_send_budget_t *b, const char *key,
                                                    size_t klen) {
    for (size_t i = 0; i < HU_SEND_BUDGET_MAX_CONTACTS; i++) {
        const hu_send_budget_contact_t *c = &b->contacts[i];
        if (c->key[0] && strlen(c->key) == klen && memcmp(c->key, key, klen) == 0)
            return c;
    }
    return NULL;
}

/* Find or claim a slot; when full, evict the contact whose most recent send
 * is oldest — it is the one least likely to be mid-conversation. */
static hu_send_budget_contact_t *find_or_claim_contact(hu_send_budget_t *b, const char *key,
                                                       size_t klen) {
    hu_send_budget_contact_t *victim = NULL;
    for (size_t i = 0; i < HU_SEND_BUDGET_MAX_CONTACTS; i++) {
        hu_send_budget_contact_t *c = &b->contacts[i];
        if (c->key[0] && strlen(c->key) == klen && memcmp(c->key, key, klen) == 0)
            return c;
        if (!c->key[0]) {
            if (!victim || victim->key[0])
                victim = c; /* prefer an empty slot over any eviction */
        } else if (!victim || (victim->key[0] && c->last_sent < victim->last_sent)) {
            victim = c;
        }
    }
    memset(victim, 0, sizeof(*victim));
    memcpy(victim->key, key, klen);
    victim->key[klen] = '\0';
    return victim;
}

/* Fill the caller's out-params for one verdict and return it, so the
 * evaluator can `return report(...)` from either scope. */
static bool report(hu_send_budget_reason_t *why, uint32_t *used, uint32_t *cap,
                   hu_send_budget_reason_t reason, uint32_t n, uint32_t limit) {
    if (why)
        *why = reason;
    if (used)
        *used = n;
    if (cap)
        *cap = limit;
    return reason == HU_SEND_BUDGET_OK;
}

static bool budget_eval(const hu_send_budget_t *b, const char *contact, size_t contact_len,
                        int64_t now, hu_send_budget_reason_t *why, uint32_t *used, uint32_t *cap) {
    if (!b)
        return report(why, used, cap, HU_SEND_BUDGET_OK, 0, 0);

    if (b->global_hourly > 0) {
        uint32_t g = count_in_window(b->global_sent_at, HU_SEND_BUDGET_MAX_GLOBAL, now);
        if (g >= b->global_hourly)
            return report(why, used, cap, HU_SEND_BUDGET_GLOBAL_EXHAUSTED, g, b->global_hourly);
    }

    size_t klen = key_len_capped(contact, contact_len);
    if (b->per_contact_hourly > 0 && klen > 0) {
        const hu_send_budget_contact_t *c = find_contact(b, contact, klen);
        if (c) {
            uint32_t n = count_in_window(c->sent_at, HU_SEND_BUDGET_MAX_PER_CONTACT, now);
            if (n >= b->per_contact_hourly)
                return report(why, used, cap, HU_SEND_BUDGET_CONTACT_EXHAUSTED, n,
                              b->per_contact_hourly);
        }
    }
    return report(why, used, cap, HU_SEND_BUDGET_OK, 0, 0);
}

bool hu_send_budget_allows(const hu_send_budget_t *b, const char *contact, size_t contact_len,
                           int64_t now, hu_send_budget_reason_t *why) {
    return budget_eval(b, contact, contact_len, now, why, NULL, NULL);
}

void hu_send_budget_record(hu_send_budget_t *b, const char *contact, size_t contact_len,
                           int64_t now) {
    if (!b || now <= 0)
        return;
    b->global_sent_at[b->global_next] = now;
    b->global_next = (b->global_next + 1) % HU_SEND_BUDGET_MAX_GLOBAL;

    size_t klen = key_len_capped(contact, contact_len);
    if (klen == 0)
        return;
    hu_send_budget_contact_t *c = find_or_claim_contact(b, contact, klen);
    c->sent_at[c->next] = now;
    c->next = (c->next + 1) % HU_SEND_BUDGET_MAX_PER_CONTACT;
    if (now > c->last_sent)
        c->last_sent = now;
}

/* ── Module singleton ─────────────────────────────────────────────────── */

static hu_send_budget_t g_send_budget;
static bool g_send_budget_ready = false;

void hu_send_budget_configure(uint32_t per_contact_hourly, uint32_t global_hourly) {
    if (!g_send_budget_ready) {
        hu_send_budget_init(&g_send_budget, per_contact_hourly, global_hourly);
        g_send_budget_ready = true;
        return;
    }
    /* Re-configure keeps history (config reload), only the limits change. */
    g_send_budget.per_contact_hourly = per_contact_hourly;
    g_send_budget.global_hourly = global_hourly;
}

bool hu_send_budget_check(const char *contact, size_t contact_len, int64_t now,
                          hu_send_budget_reason_t *why, uint32_t *used, uint32_t *cap) {
    if (!g_send_budget_ready)
        hu_send_budget_configure(HU_SEND_BUDGET_DEFAULT_PER_CONTACT, HU_SEND_BUDGET_DEFAULT_GLOBAL);
    return budget_eval(&g_send_budget, contact, contact_len, now, why, used, cap);
}

void hu_send_budget_record_send(const char *contact, size_t contact_len, int64_t now) {
    if (!g_send_budget_ready)
        hu_send_budget_configure(HU_SEND_BUDGET_DEFAULT_PER_CONTACT, HU_SEND_BUDGET_DEFAULT_GLOBAL);
    hu_send_budget_record(&g_send_budget, contact, contact_len, now);
}

void hu_send_budget_reset(void) {
    memset(&g_send_budget, 0, sizeof(g_send_budget));
    g_send_budget_ready = false;
}
