/* src/daemon/daemon_spontaneity.c — the reactive path's spontaneous extras
 * (double-text afterthought, self-reaction, GIF) and the HU_SPONTANEITY gate
 * that makes them reachable and decides them from learned, outcome-updated
 * posteriors. Contract and rationale: include/human/daemon/spontaneity.h.
 * The OFF/SHADOW bodies are the code that used to sit inline in src/daemon.c. */
/* usleep / getpid are XSI/BSD; glibc declares them only with these. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "human/agent.h"
#include "human/agent/contextual_bandit.h"
#include "human/agent/humanization_bandit.h"
#include "human/agent/reaction_handler.h"
#include "human/agent/validators/builtin.h"
#include "human/config.h"
#include "human/contact_send_recency.h"
#include "human/context/conversation.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/log_redact.h"
#include "human/core/paths.h"
#include "human/core/state_file.h"
#include "human/core/string.h"
#include "human/daemon/spontaneity.h"
#include "human/daemon_routing.h"
#include "human/persona.h"
#include "human/persona/card_file.h"
#ifdef HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *const k_kind_name[HU_SPONT_KIND_COUNT] = {"double_text", "self_reaction", "gif"};
static const char *const k_rate_key[HU_SPONT_KIND_COUNT] = {"double_text_rate",
                                                            "self_reaction_rate", "gif_rate"};
static const char *const k_n_key[HU_SPONT_KIND_COUNT] = {"double_text_n", "self_reaction_n",
                                                         "gif_n"};

/* The learned rates are re-read at most this often (the file is rewritten by
 * the offline learner, never by the daemon). */
#define HU_SPONT_RATES_TTL_S 600
/* A tapped message counts as "the extra or later" when it was sent no earlier
 * than this before the extra's delivery stamp (the same tolerance the DEF-8
 * join uses between outbound_sends and chat.db, measured 2026-10-02). */
#define HU_SPONT_DELIVERY_SLACK_MS 5000

#define SPONT_QUEUE_CAP   8
#define SPONT_PENDING_CAP 32

/* A LIVE extra waiting for the next daemon pass (no sleeping on the loop). */
typedef struct {
    bool active;
    hu_spontaneity_kind_t kind;
    char channel[32];
    char target[128];  /* send target */
    char contact[128]; /* batch key: rowid lookup, rate/cal bookkeeping */
    int64_t fallback_sent_id;
    hu_reaction_type_t reaction;
    char text[512]; /* double-text body or GIF query */
    size_t text_len;
    char path[512]; /* GIF file */
    int64_t due_ms;
} spont_queued_t;

/* An extra that fired, awaiting its outcome. delivered_ms 0 = not yet out. */
typedef struct {
    bool active;
    hu_spontaneity_kind_t kind;
    char contact[128];
    int64_t delivered_ms;
} spont_pending_t;

static spont_queued_t s_queue[SPONT_QUEUE_CAP];
static spont_pending_t s_pending[SPONT_PENDING_CAP];
static hu_contextual_bandit_t *s_outcomes; /* successes / failures per (contact, kind) */
static bool s_outcomes_loaded;

#if HU_IS_TEST
static int s_mode_override = -1;
static bool s_rates_override_set = false;
static hu_spontaneity_rates_t s_rates_override;
#endif
static uint32_t s_rng = 0;

hu_gate_mode_t hu_spontaneity_mode(void) {
#if HU_IS_TEST
    if (s_mode_override >= 0)
        return (hu_gate_mode_t)s_mode_override;
#endif
    return hu_gate_mode_from_env("HU_SPONTANEITY", HU_GATE_OFF);
}

static int64_t now_ms_wall(void) {
    return (int64_t)time(NULL) * 1000;
}

static void copy_str(char *dst, size_t cap, const char *src, size_t len) {
    if (!src)
        len = 0;
    if (len >= cap)
        len = cap - 1;
    if (len)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ── Learned rates ─────────────────────────────────────────────────────── */

static void rates_clear(hu_spontaneity_rates_t *r) {
    for (int k = 0; k < HU_SPONT_KIND_COUNT; k++) {
        r->rate[k] = -1.0;
        r->n0[k] = -1.0;
    }
    r->double_text_gap_s = -1.0;
}

static double rate_field(const hu_json_value_t *global, const hu_json_value_t *root,
                         const char *key) {
    double v = global ? hu_json_get_number(global, key, -1.0) : -1.0;
    if (!(v >= 0.0))
        v = hu_json_get_number(root, key, -1.0);
    return v;
}

hu_error_t hu_spontaneity_rates_parse(hu_allocator_t *alloc, const char *json, size_t len,
                                      hu_spontaneity_rates_t *out) {
    if (!alloc || !json || !out)
        return HU_ERR_INVALID_ARGUMENT;
    rates_clear(out);
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_persona_card_parse_object(alloc, json, len, &root);
    if (err != HU_OK)
        return HU_ERR_PARSE;
    /* learned-style/v1 keeps per-reply stats in the "global" block
     * (docs/guides/learned-style.md); a top-level field is also accepted. */
    const hu_json_value_t *global = hu_json_object_get(root, "global");
    if (global && global->type != HU_JSON_OBJECT)
        global = NULL;
    double n_fallback = rate_field(global, root, "n_eff");
    if (!(n_fallback > 0.0))
        n_fallback = rate_field(global, root, "n");
    for (int k = 0; k < HU_SPONT_KIND_COUNT; k++) {
        double v = rate_field(global, root, k_rate_key[k]);
        if (!(v >= 0.0 && v <= 1.0))
            continue; /* null / absent / out of range = not measured */
        double n0 = rate_field(global, root, k_n_key[k]);
        if (!(n0 > 0.0))
            n0 = n_fallback;
        if (!(n0 > 0.0))
            continue; /* a rate with no sample size cannot seed a posterior */
        out->rate[k] = v;
        out->n0[k] = n0;
    }
    double gap = rate_field(global, root, "double_text_gap_s");
    if (gap >= 0.0)
        out->double_text_gap_s = gap;
    hu_json_free(alloc, root);
    return HU_OK;
}

static void rates_for(const struct hu_agent *agent, hu_spontaneity_rates_t *out) {
#if HU_IS_TEST
    if (s_rates_override_set) {
        *out = s_rates_override;
        return;
    }
#endif
    static hu_spontaneity_rates_t cached;
    static int64_t cached_at = 0;
    int64_t now = (int64_t)time(NULL);
    if (cached_at != 0 && now - cached_at < HU_SPONT_RATES_TTL_S) {
        *out = cached;
        return;
    }
    rates_clear(&cached);
    cached_at = now;
    if (agent && agent->persona_name && agent->persona_name_len > 0) {
        hu_allocator_t alloc = hu_system_allocator();
        char *buf = NULL;
        size_t got = 0;
        if (hu_persona_card_slurp(&alloc, agent->persona_name, agent->persona_name_len,
                                  ".learned-style.json", &buf, &got) == HU_OK) {
            (void)hu_spontaneity_rates_parse(&alloc, buf, got, &cached);
            alloc.free(alloc.ctx, buf, got + 1);
        }
    }
    *out = cached;
}

/* ── Outcome posterior: one Beta arm per (contact, kind) ───────────────── */

static uint64_t arm_key(const char *contact, size_t len, hu_spontaneity_kind_t kind) {
    char key[256];
    copy_str(key, sizeof(key), contact, len);
    uint64_t h = hu_contact_handle_hash(key) ^ ((uint64_t)(kind + 1) * 0x9E3779B97F4A7C15ULL);
    return h ? h : 1;
}

static hu_contextual_bandit_t *outcomes(void) {
    if (!s_outcomes) {
        static hu_allocator_t alloc;
        alloc = hu_system_allocator();
        if (hu_contextual_bandit_create(&alloc, 512, &s_outcomes) != HU_OK)
            return NULL;
    }
    if (!s_outcomes_loaded) {
        s_outcomes_loaded = true;
        char path[512];
        if (hu_state_file_default_path("bandit_spontaneity.json", path, sizeof(path)))
            (void)hu_humanization_bandit_load_file(s_outcomes, path);
    }
    return s_outcomes;
}

/* Arms start at Beta(1,1); alpha-1 / beta-1 are the outcome counts. */
static void arm_counts(const char *contact, size_t len, hu_spontaneity_kind_t kind, double *succ,
                       double *fail) {
    *succ = 0.0;
    *fail = 0.0;
    hu_contextual_bandit_t *b = outcomes();
    hu_contextual_bandit_arm_t arm;
    if (b && hu_contextual_bandit_get_arm(b, arm_key(contact, len, kind), &arm) == HU_OK) {
        *succ = arm.alpha - 1.0;
        *fail = arm.beta - 1.0;
    }
}

static void record_outcome(const spont_pending_t *p, bool success) {
    hu_contextual_bandit_t *b = outcomes();
    if (!b)
        return;
    (void)hu_contextual_bandit_update(b, arm_key(p->contact, strlen(p->contact), p->kind),
                                      success ? HU_BANDIT_REPLY : HU_BANDIT_IGNORED);
    hu_log_info("spontaneity", NULL, "[HU_SPONTANEITY outcome] kind=%s success=%d",
                k_kind_name[p->kind], success ? 1 : 0);
    char path[512];
    if (hu_state_file_default_path("bandit_spontaneity.json", path, sizeof(path)))
        (void)hu_humanization_bandit_save_file(b, path);
}

/* xorshift32; seeded once per process (tests seed it explicitly). */
static uint32_t rng_next(void) {
    if (s_rng == 0)
        s_rng = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ 0x9E3779B9u;
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static double uniform01(void) {
    return (double)(rng_next() >> 8) / (double)(1u << 24);
}

double hu_spontaneity_sample_p(const char *contact, size_t contact_len, hu_spontaneity_kind_t kind,
                               const hu_spontaneity_rates_t *rates) {
    if (!rates || kind >= HU_SPONT_KIND_COUNT || !(rates->rate[kind] >= 0.0) ||
        !(rates->n0[kind] > 0.0))
        return -1.0;
    double succ = 0.0, fail = 0.0;
    arm_counts(contact, contact_len, kind, &succ, &fail);
    double a = rates->rate[kind] * rates->n0[kind] + succ;
    double b = (1.0 - rates->rate[kind]) * rates->n0[kind] + fail;
    if (a <= 0.0)
        return 0.0; /* Seth never does it and no outcome says otherwise */
    if (b <= 0.0)
        return 1.0;
    uint32_t seed = rng_next();
    return hu_contextual_bandit_sample_beta(a, b, &seed);
}

/* ── Queue + pending bookkeeping ───────────────────────────────────────── */

static void pending_add(const char *contact, size_t len, hu_spontaneity_kind_t kind,
                        int64_t delivered_ms) {
    spont_pending_t *slot = NULL;
    for (size_t i = 0; i < SPONT_PENDING_CAP && !slot; i++)
        if (!s_pending[i].active)
            slot = &s_pending[i];
    if (!slot)
        return; /* full: this extra goes unlearned rather than evicting one */
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->kind = kind;
    copy_str(slot->contact, sizeof(slot->contact), contact, len);
    slot->delivered_ms = delivered_ms;
}

static void pending_mark_delivered(const char *contact, hu_spontaneity_kind_t kind, int64_t now) {
    for (size_t i = 0; i < SPONT_PENDING_CAP; i++) {
        spont_pending_t *p = &s_pending[i];
        if (p->active && p->kind == kind && p->delivered_ms == 0 &&
            strcmp(p->contact, contact) == 0)
            p->delivered_ms = now;
    }
}

static void pending_drop_undelivered(const char *contact, hu_spontaneity_kind_t kind) {
    for (size_t i = 0; i < SPONT_PENDING_CAP; i++) {
        spont_pending_t *p = &s_pending[i];
        if (p->active && p->kind == kind && p->delivered_ms == 0 &&
            strcmp(p->contact, contact) == 0)
            p->active = false;
    }
}

static spont_queued_t *queue_slot(void) {
    for (size_t i = 0; i < SPONT_QUEUE_CAP; i++)
        if (!s_queue[i].active) {
            memset(&s_queue[i], 0, sizeof(s_queue[i]));
            return &s_queue[i];
        }
    return NULL;
}

static bool queue_for(const hu_spontaneity_turn_t *t, hu_spontaneity_kind_t kind,
                      spont_queued_t **out) {
    spont_queued_t *q = queue_slot();
    const char *ch = t->channel->vtable->name ? t->channel->vtable->name(t->channel->ctx) : NULL;
    if (!q || !ch)
        return false;
    q->active = true;
    q->kind = kind;
    copy_str(q->channel, sizeof(q->channel), ch, strlen(ch));
    copy_str(q->target, sizeof(q->target), t->send_target, t->send_target_len);
    copy_str(q->contact, sizeof(q->contact), t->contact, t->contact_len);
    q->fallback_sent_id = t->fallback_sent_id;
    q->due_ms = now_ms_wall(); /* the next daemon pass: no sleep on this one */
    pending_add(t->contact, t->contact_len, kind, 0);
    *out = q;
    return true;
}

/* Success for every delivered extra to `key` that went out no later than
 * `hi` (the engagement came after it). */
static void credit_delivered(const char *key, int64_t hi) {
    for (size_t i = 0; i < SPONT_PENDING_CAP; i++) {
        spont_pending_t *p = &s_pending[i];
        if (p->active && p->delivered_ms > 0 && p->delivered_ms <= hi &&
            strcmp(p->contact, key) == 0) {
            record_outcome(p, true);
            p->active = false;
        }
    }
}

void hu_daemon_spontaneity_on_inbound(const char *contact, size_t contact_len, int64_t now_ms) {
    if (!contact || contact_len == 0)
        return;
    char key[128];
    copy_str(key, sizeof(key), contact, contact_len);
    /* An extra still queued when they write back is moot: one emission per
     * turn, so it is cancelled, and it teaches nothing. */
    for (size_t i = 0; i < SPONT_QUEUE_CAP; i++) {
        if (s_queue[i].active && strcmp(s_queue[i].contact, key) == 0) {
            pending_drop_undelivered(key, s_queue[i].kind);
            s_queue[i].active = false;
        }
    }
    credit_delivered(key, now_ms);
}

void hu_daemon_spontaneity_on_engagement(const char *contact, size_t contact_len, int64_t sent_ms) {
    if (!contact || contact_len == 0)
        return;
    char key[128];
    copy_str(key, sizeof(key), contact, contact_len);
    /* Only a tap on the extra itself or a later message of ours counts. */
    credit_delivered(key, sent_ms + HU_SPONT_DELIVERY_SLACK_MS);
}

/* ── Turn decision ─────────────────────────────────────────────────────── */

static const hu_provider_vtable_t *dt_provider(const hu_spontaneity_turn_t *t, void **ctx) {
    const hu_provider_vtable_t *vt = t->classify ? t->classify->vtable : t->agent->provider.vtable;
    *ctx = t->classify ? t->classify->ctx : t->agent->provider.ctx;
    return (vt && vt->chat_with_system) ? vt : NULL;
}

/* GIF rate cap the daemon applies: 5 per 10 minutes per contact. */
static bool gif_rate_ok(const hu_spontaneity_turn_t *t, uint64_t now_ms) {
    return hu_conversation_gif_rate_allow(t->contact, t->contact_len, now_ms, 5, 600000);
}

/* Klipy (Tenor's v2 contract) is the GIF source the daemon's GIF block uses. */
static bool gif_provider_configured(const struct hu_config *config) {
    const char *key = config ? hu_config_get_provider_key(config, "klipy") : NULL;
    return key && key[0];
}

static bool eligible(const hu_spontaneity_turn_t *t, hu_spontaneity_kind_t kind) {
    void *ctx = NULL;
    switch (kind) {
    case HU_SPONT_DOUBLE_TEXT:
        return t->agent && t->agent->persona && t->response && t->response_len > 0 &&
               t->channel->vtable->send && dt_provider(t, &ctx) &&
               hu_conversation_should_double_text(t->response, t->response_len, t->history,
                                                  t->history_count, t->hour_local, 0, 1.0f);
    case HU_SPONT_SELF_REACTION:
        return t->response && t->response_len > 0 && t->channel->vtable->react && !t->is_group &&
               hu_conversation_self_reaction_kind(t->response, t->response_len) != HU_REACTION_NONE;
    case HU_SPONT_GIF:
        return t->inbound && t->inbound_len > 0 && t->channel->vtable->send &&
               (t->gif_available || gif_provider_configured(t->config)) &&
               hu_conversation_should_send_gif(t->inbound, t->inbound_len, t->history,
                                               t->history_count, 0, 1.0f) &&
               gif_rate_ok(t, (uint64_t)now_ms_wall());
    default:
        return false;
    }
}

hu_spontaneity_kind_t hu_daemon_spontaneity_choose(hu_spontaneity_turn_t *t) {
    if (!t)
        return HU_SPONT_NONE;
    t->chosen = HU_SPONT_NONE;
    hu_gate_mode_t mode = hu_spontaneity_mode();
    if (mode == HU_GATE_OFF || !t->channel || !t->channel->vtable)
        return HU_SPONT_NONE;
    /* Tapbacks on our messages credit fired extras (DEF-8 join). */
    hu_reaction_handler_set_engagement_sink(hu_daemon_spontaneity_on_engagement);

    hu_spontaneity_rates_t rates;
    rates_for(t->agent, &rates);
    int elig[HU_SPONT_KIND_COUNT] = {0}, fire[HU_SPONT_KIND_COUNT] = {0};
    double p[HU_SPONT_KIND_COUNT] = {0};
    hu_spontaneity_kind_t fired[HU_SPONT_KIND_COUNT];
    size_t n_fired = 0;
    for (int k = 0; k < HU_SPONT_KIND_COUNT; k++) {
        elig[k] = eligible(t, (hu_spontaneity_kind_t)k) ? 1 : 0;
        p[k] = elig[k] ? hu_spontaneity_sample_p(t->contact, t->contact_len,
                                                 (hu_spontaneity_kind_t)k, &rates)
                       : -1.0;
        fire[k] = (elig[k] && p[k] >= 0.0 && uniform01() < p[k]) ? 1 : 0;
        if (fire[k])
            fired[n_fired++] = (hu_spontaneity_kind_t)k;
    }
    /* Which extra, when several fire: uniform across them, never a fixed order. */
    hu_spontaneity_kind_t pick = n_fired ? fired[rng_next() % n_fired] : HU_SPONT_NONE;
    bool live = mode == HU_GATE_LIVE;
    hu_log_info("spontaneity", NULL,
                "[HU_SPONTANEITY %s] turn=1 dt=%d/%.3f/%d sr=%d/%.3f/%d gif=%d/%.3f/%d chosen=%s "
                "fired=%d",
                live ? "live" : "shadow", elig[0], p[0], fire[0], elig[1], p[1], fire[1], elig[2],
                p[2], fire[2], pick == HU_SPONT_NONE ? "none" : k_kind_name[pick],
                (live && pick != HU_SPONT_NONE) ? 1 : 0);
    if (live)
        t->chosen = pick;
    return pick;
}

/* ── F9 double-text afterthought ─────────────────────────────────────────── */

/* Generate the afterthought into out (validated and shaped like every other
 * daemon-originated bubble). Returns its length, 0 on failure. */
static size_t double_text_generate(hu_spontaneity_turn_t *t, uint32_t dt_seed, char *out,
                                   size_t cap) {
    struct hu_agent *agent = t->agent;
    hu_allocator_t *alloc = t->alloc;
    void *dt_ctx = NULL;
    const hu_provider_vtable_t *dt_vtable = dt_provider(t, &dt_ctx);
    char dt_user[512];
    int dt_n = snprintf(dt_user, sizeof(dt_user),
                        "You just sent this message: \"%.*s\"\n"
                        "Add a brief, natural follow-up thought (1 short sentence max). "
                        "Something you'd double-text a moment later.",
                        (int)(t->response_len > 200 ? 200 : t->response_len), t->response);
    if (!dt_vtable || dt_n <= 0 || (size_t)dt_n >= sizeof(dt_user))
        return 0;
    char *dt_resp = NULL;
    size_t dt_resp_len = 0;
    size_t dt_fb_len = 0;
    const char *dt_fb = hu_daemon_fallback_model(t->config, &dt_fb_len);
    const char *dt_model =
        t->classify ? t->classify_model : (agent->model_name ? agent->model_name : dt_fb);
    size_t dt_model_len = t->classify ? t->classify_model_len
                                      : (agent->model_name ? agent->model_name_len : dt_fb_len);
    hu_error_t dt_err = dt_vtable->chat_with_system(
        dt_ctx, alloc,
        "You are texting as this person. Keep it casual, short, "
        "lowercase. "
        "No quotes, no explanation, just the follow-up text.",
        93, dt_user, (size_t)dt_n, dt_model, dt_model_len, 0.9, &dt_resp, &dt_resp_len);
    size_t out_len = 0;
    if (dt_err == HU_OK && dt_resp && dt_resp_len > 0 && dt_resp_len < 200) {
        /* Post-process double-text through the same BTH pipeline */
        hu_validator_chain_apply_default_in_place(alloc, agent ? agent->observer : NULL, NULL, 0,
                                                  "double-text send", dt_resp, &dt_resp_len,
                                                  dt_resp_len + 1);
        if (dt_resp_len > 0) {
            dt_resp_len = hu_conversation_vary_complexity(dt_resp, dt_resp_len, dt_seed);
            if (dt_resp_len > 1 && dt_resp[0] >= 'A' && dt_resp[0] <= 'Z' && dt_resp[1] >= 'a' &&
                dt_resp[1] <= 'z' && dt_resp[0] != 'I') {
                dt_resp[0] = (char)(dt_resp[0] + 32);
            }
            if (dt_resp_len > 1 && dt_resp[dt_resp_len - 1] == '.') {
                dt_resp[dt_resp_len - 1] = '\0';
                dt_resp_len--;
            }
            copy_str(out, cap, dt_resp, dt_resp_len);
            out_len = strlen(out);
        }
    }
    if (dt_resp)
        alloc->free(alloc->ctx, dt_resp, dt_resp_len + 1);
    return out_len;
}

void hu_daemon_spontaneity_double_text(hu_spontaneity_turn_t *t) {
    if (!t || !t->agent || !t->channel)
        return;
    struct hu_agent *agent = t->agent;
    uint32_t dt_seed =
        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)t->response;
    char text[512];
    if (hu_spontaneity_mode() == HU_GATE_LIVE) {
        if (t->chosen != HU_SPONT_DOUBLE_TEXT)
            return;
        spont_queued_t *q = NULL;
        size_t n = double_text_generate(t, dt_seed, text, sizeof(text));
        if (n == 0 || !queue_for(t, HU_SPONT_DOUBLE_TEXT, &q))
            return;
        copy_str(q->text, sizeof(q->text), text, n);
        q->text_len = strlen(q->text);
        /* Seth's own afterthought gap when measured; else the next daemon pass. */
        hu_spontaneity_rates_t rates;
        rates_for(t->agent, &rates);
        if (rates.double_text_gap_s > 0.0)
            q->due_ms += (int64_t)(rates.double_text_gap_s * 1000.0);
        return;
    }
    /* OFF / SHADOW: the original one-emission-per-turn path (unreachable in
     * practice: the reactive send was recorded just before this). */
    void *ctx = NULL;
    if (!t->response || t->response_len == 0 || !agent->persona || !t->channel->vtable->send ||
        !dt_provider(t, &ctx) ||
        hu_daemon_proactive_should_defer(&agent->contact_send_recency, t->contact, t->contact_len,
                                         (int64_t)time(NULL)))
        return;
    float dt_prob = agent->persona->humanization.double_text_probability;
    if (!hu_conversation_should_double_text(t->response, t->response_len, t->history,
                                            t->history_count, t->hour_local, dt_seed, dt_prob))
        return;
    size_t n = double_text_generate(t, dt_seed, text, sizeof(text));
    if (n == 0)
        return;
    unsigned int dt_delay = 10000u + (dt_seed % 35000u);
    usleep((useconds_t)(dt_delay * 1000u));
    hu_error_t send_err = t->channel->vtable->send(t->channel->ctx, t->send_target,
                                                   t->send_target_len, text, n, NULL, 0);
    if (send_err == HU_OK && agent->bth_metrics)
        agent->bth_metrics->double_texts++; /* count delivered afterthoughts only */
}

/* ── Self-reaction (haha / emphasize our own message) ────────────────────── */

static int64_t latest_sent_id(const char *contact, int64_t fallback) {
#ifdef HU_HAS_IMESSAGE
    int64_t id = hu_imessage_get_latest_sent_rowid(contact, strlen(contact));
#if HU_IS_TEST
    if (id <= 0)
        id = fallback; /* no chat.db under test */
#else
    (void)fallback;
#endif
    return id;
#else
    (void)contact;
    return fallback;
#endif
}

void hu_daemon_spontaneity_self_reaction(hu_spontaneity_turn_t *t) {
    /* Skip for groups: get_latest_sent_rowid uses handle.id SQL. */
    if (!t || !t->channel || !t->response || t->response_len == 0 || !t->channel->vtable->react ||
        t->is_group)
        return;
    if (hu_spontaneity_mode() == HU_GATE_LIVE) {
        spont_queued_t *q = NULL;
        if (t->chosen != HU_SPONT_SELF_REACTION || !queue_for(t, HU_SPONT_SELF_REACTION, &q))
            return;
        q->reaction = hu_conversation_self_reaction_kind(t->response, t->response_len);
        return;
    }
    if (t->agent && hu_daemon_proactive_should_defer(&t->agent->contact_send_recency, t->contact,
                                                     t->contact_len, (int64_t)time(NULL)))
        return;
    hu_reaction_type_t self_r =
        hu_conversation_classify_self_reaction(t->response, t->response_len, (uint32_t)time(NULL));
    if (self_r == HU_REACTION_NONE)
        return;
    usleep(1500000 + ((uint32_t)time(NULL) % 3000000));
    char key[128];
    copy_str(key, sizeof(key), t->contact, t->contact_len);
    int64_t sent_id = latest_sent_id(key, t->fallback_sent_id);
    if (sent_id > 0) {
        t->channel->vtable->react(t->channel->ctx, t->send_target, t->send_target_len, sent_id,
                                  self_r);
        hu_log_info("human", t->agent ? t->agent->observer : NULL,
                    "self-reaction on own message: %d", (int)self_r);
    }
}

/* ── GIF ─────────────────────────────────────────────────────────────────── */

bool hu_daemon_spontaneity_gif_open(hu_spontaneity_turn_t *t, uint64_t now_ms) {
    (void)now_ms;
    if (!t)
        return false;
    if (hu_spontaneity_mode() == HU_GATE_LIVE)
        return t->chosen == HU_SPONT_GIF;
    return !(t->agent &&
             hu_daemon_proactive_should_defer(&t->agent->contact_send_recency, t->contact,
                                              t->contact_len, (int64_t)time(NULL)));
}

bool hu_daemon_spontaneity_gif_roll(hu_spontaneity_turn_t *t, uint32_t seed, float legacy_prob,
                                    uint64_t now_ms) {
    if (!t)
        return false;
    if (hu_spontaneity_mode() == HU_GATE_LIVE)
        return t->chosen == HU_SPONT_GIF; /* eligibility + rate cap decided in choose */
    return hu_conversation_should_send_gif(t->inbound, t->inbound_len, t->history, t->history_count,
                                           seed, legacy_prob) &&
           gif_rate_ok(t, now_ms);
}

static void gif_bookkeeping(const char *contact, const char *query, size_t query_len,
                            uint64_t now_ms) {
    size_t clen = strlen(contact);
    hu_conversation_gif_rate_record(contact, clen, now_ms);
    hu_conversation_gif_cal_record_send(contact, clen, query, query_len);
    char cal_path[512];
    int cp_n = hu_paths_state(cal_path, sizeof(cal_path), "gif_calibration.json");
    if (cp_n > 0 && (size_t)cp_n < sizeof(cal_path))
        hu_conversation_gif_cal_save(cal_path, (size_t)cp_n);
}

bool hu_daemon_spontaneity_gif_send(hu_spontaneity_turn_t *t, char *gif_path, const char *query,
                                    size_t query_len, uint32_t seed, uint64_t now_ms) {
    if (!t || !gif_path)
        return false;
    size_t path_len = strlen(gif_path);
    bool done = false;
    if (hu_spontaneity_mode() == HU_GATE_LIVE) {
        spont_queued_t *q = NULL;
        if (path_len < sizeof(q->path) && queue_for(t, HU_SPONT_GIF, &q)) {
            copy_str(q->path, sizeof(q->path), gif_path, path_len);
            copy_str(q->text, sizeof(q->text), query, query_len);
            q->text_len = strlen(q->text);
            done = true;
        } else {
            (void)unlink(gif_path);
        }
    } else {
        usleep(2000000 + (seed % 3000000));
        const char *media[] = {gif_path};
        if (t->channel->vtable->send(t->channel->ctx, t->contact, t->contact_len, "", 0, media,
                                     1) != HU_OK)
            hu_log_warn("spontaneity", NULL, "GIF send failed"); /* bookkeeping as before */
        (void)unlink(gif_path);
        char key[128];
        copy_str(key, sizeof(key), t->contact, t->contact_len);
        gif_bookkeeping(key, query, query_len, now_ms);
        hu_log_info("human", t->agent ? t->agent->observer : NULL, "sent GIF: query=\"%s\"",
                    HU_LOG_TEXT(query, query_len, 120)); /* #596 log privacy */
        done = true;
    }
    t->alloc->free(t->alloc->ctx, gif_path, path_len + 1);
    return done;
}

/* ── Delivery + expiry pass ──────────────────────────────────────────────── */

static bool deliver(hu_channel_t *channel, spont_queued_t *q) {
    hu_error_t err = HU_ERR_NOT_SUPPORTED;
    switch (q->kind) {
    case HU_SPONT_DOUBLE_TEXT:
        if (channel->vtable->send)
            err = channel->vtable->send(channel->ctx, q->target, strlen(q->target), q->text,
                                        q->text_len, NULL, 0);
        return err == HU_OK;
    case HU_SPONT_SELF_REACTION: {
        int64_t id = latest_sent_id(q->contact, q->fallback_sent_id);
        if (id > 0 && channel->vtable->react)
            err =
                channel->vtable->react(channel->ctx, q->target, strlen(q->target), id, q->reaction);
        return err == HU_OK;
    }
    case HU_SPONT_GIF: {
        const char *media[] = {q->path};
        if (channel->vtable->send)
            err = channel->vtable->send(channel->ctx, q->contact, strlen(q->contact), "", 0, media,
                                        1);
        (void)unlink(q->path);
        if (err == HU_OK)
            gif_bookkeeping(q->contact, q->text, q->text_len, (uint64_t)now_ms_wall());
        return err == HU_OK;
    }
    default:
        return false;
    }
}

void hu_daemon_spontaneity_tick(hu_channel_t *channel, const char *channel_name, int64_t now_ms) {
    if (channel && channel->vtable && channel_name) {
        for (size_t i = 0; i < SPONT_QUEUE_CAP; i++) {
            spont_queued_t *q = &s_queue[i];
            if (!q->active || q->due_ms > now_ms || strcmp(q->channel, channel_name) != 0)
                continue;
            q->active = false;
            bool ok = deliver(channel, q);
            if (ok)
                pending_mark_delivered(q->contact, q->kind, now_ms);
            else
                pending_drop_undelivered(q->contact, q->kind);
            hu_log_info("spontaneity", NULL, "[HU_SPONTANEITY live] kind=%s delivered=%d",
                        k_kind_name[q->kind], ok ? 1 : 0);
        }
    }
    for (size_t i = 0; i < SPONT_PENDING_CAP; i++) {
        spont_pending_t *p = &s_pending[i];
        if (p->active && p->delivered_ms > 0 &&
            now_ms - p->delivered_ms > (int64_t)HU_SPONT_OUTCOME_HORIZON_S * 1000) {
            record_outcome(p, false); /* silence for the horizon */
            p->active = false;
        }
    }
}

#if HU_IS_TEST
void hu_spontaneity_set_for_test(int mode, const hu_spontaneity_rates_t *rates, uint32_t seed) {
    s_mode_override = mode;
    s_rates_override_set = rates != NULL;
    if (rates)
        s_rates_override = *rates;
    s_rng = seed;
    memset(s_queue, 0, sizeof(s_queue));
    memset(s_pending, 0, sizeof(s_pending));
    if (s_outcomes) {
        hu_contextual_bandit_destroy(s_outcomes);
        s_outcomes = NULL;
    }
    s_outcomes_loaded = false;
    hu_reaction_handler_set_engagement_sink(NULL);
}

void hu_spontaneity_outcomes_for_test(const char *contact, hu_spontaneity_kind_t kind,
                                      uint64_t *succ, uint64_t *fail) {
    double s = 0.0, f = 0.0;
    arm_counts(contact, strlen(contact), kind, &s, &f);
    *succ = (uint64_t)s;
    *fail = (uint64_t)f;
}

size_t hu_spontaneity_queued_for_test(void) {
    size_t n = 0;
    for (size_t i = 0; i < SPONT_QUEUE_CAP; i++)
        n += s_queue[i].active ? 1 : 0;
    return n;
}
#endif
