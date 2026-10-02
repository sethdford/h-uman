/* src/daemon/daemon_spontaneity.c — the reactive path's spontaneous extras
 * (double-text afterthought, self-reaction, GIF) and the HU_SPONTANEITY gate
 * that makes them reachable and decides them from learned rates. Contract and
 * rationale: include/human/daemon/spontaneity.h. The double-text and
 * self-reaction bodies moved here verbatim from src/daemon.c (DEF-15). */
/* usleep / getpid are XSI/BSD; glibc declares them only with these. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "human/agent.h"
#include "human/agent/contextual_bandit.h"
#include "human/agent/humanization_bandit.h"
#include "human/agent/validators/builtin.h"
#include "human/contact_send_recency.h"
#include "human/context/conversation.h"
#include "human/core/json.h"
#include "human/core/log.h"
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

/* The learned rates are re-read at most this often (the file is rewritten by
 * the offline learner, never by the daemon). */
#define HU_SPONT_RATES_TTL_S 600

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

static void rates_clear(hu_spontaneity_rates_t *r) {
    for (int k = 0; k < HU_SPONT_KIND_COUNT; k++)
        r->rate[k] = -1.0;
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
    /* learned-style/v1 keeps per-reply rates in the "global" stats block
     * (docs/guides/learned-style.md); a top-level field is also accepted. */
    const hu_json_value_t *global = hu_json_object_get(root, "global");
    if (global && global->type != HU_JSON_OBJECT)
        global = NULL;
    for (int k = 0; k < HU_SPONT_KIND_COUNT; k++) {
        double v = global ? hu_json_get_number(global, k_rate_key[k], -1.0) : -1.0;
        if (!(v >= 0.0 && v <= 1.0))
            v = hu_json_get_number(root, k_rate_key[k], -1.0);
        if (v >= 0.0 && v <= 1.0)
            out->rate[k] = v;
    }
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

/* xorshift32; seeded once per process (tests seed it explicitly). */
static double uniform01(void) {
    if (s_rng == 0)
        s_rng = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ 0x9E3779B9u;
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (double)(s_rng >> 8) / (double)(1u << 24);
}

hu_spontaneity_decision_t hu_spontaneity_policy(double learned_rate, double theta, double u) {
    hu_spontaneity_decision_t d = {false, false, 0.0};
    if (!(learned_rate >= 0.0 && learned_rate <= 1.0))
        return d;
    d.have_rate = true;
    /* Bounded outcome residual: theta in [0,1] scales Seth's own rate by
     * [0.8, 1.25] (= ±log 1.25 trust region, static-rules-inventory §5.5). */
    double m = (theta >= 0.0 && theta <= 1.0) ? 0.8 + 0.45 * theta : 1.0;
    d.p = learned_rate * m;
    if (d.p > 1.0)
        d.p = 1.0;
    d.fire = u < d.p;
    return d;
}

/* theta for the contact's humanization arm; -1 when there is no bandit or the
 * contact has no outcomes yet (a fresh arm carries no information). */
static double contact_theta(const hu_spontaneity_turn_t *t) {
    if (!t->agent || !t->agent->sota.bandit || !t->contact || t->contact_len == 0)
        return -1.0;
    char key[256];
    size_t n = t->contact_len < sizeof(key) - 1 ? t->contact_len : sizeof(key) - 1;
    memcpy(key, t->contact, n);
    key[n] = '\0';
    uint64_t h = hu_contact_handle_hash(key);
    hu_contextual_bandit_arm_t arm;
    if (h == 0 || hu_contextual_bandit_get_arm(t->agent->sota.bandit, h, &arm) != HU_OK ||
        arm.updates == 0)
        return -1.0;
    double theta = -1.0;
    if (hu_humanization_bandit_sample_theta(t->agent->sota.bandit, h, &theta) != HU_OK)
        return -1.0;
    return theta;
}

/* SHADOW/LIVE decision for one eligible-or-not extra. Logs one aggregate
 * line (kind, eligibility, rate source, p, outcome) — never text or handles.
 * Returns true only when LIVE and the extra should be sent now. */
static bool spontaneity_decide(hu_spontaneity_turn_t *t, hu_spontaneity_kind_t kind, bool eligible,
                               hu_gate_mode_t mode) {
    if (mode == HU_GATE_OFF || !eligible)
        return false;
    hu_spontaneity_rates_t rates;
    rates_for(t->agent, &rates);
    hu_spontaneity_decision_t d =
        hu_spontaneity_policy(rates.rate[kind], contact_theta(t), uniform01());
    bool capped = t->fired >= 1;
    bool send = mode == HU_GATE_LIVE && d.have_rate && d.fire && !capped;
    if (!send)
        hu_log_info("spontaneity", NULL,
                    "[HU_SPONTANEITY shadow] kind=%s eligible=1 rate_src=%s p=%.3f "
                    "would_fire=%d capped=%d",
                    k_kind_name[kind], d.have_rate ? "learned" : "none", d.p, d.fire ? 1 : 0,
                    capped ? 1 : 0);
    else
        hu_log_info("spontaneity", NULL, "[HU_SPONTANEITY live] kind=%s p=%.3f fired=1",
                    k_kind_name[kind], d.p);
    if (send)
        t->fired++;
    return send;
}

static bool reactive_just_sent(const hu_spontaneity_turn_t *t) {
    return t->agent && hu_daemon_proactive_should_defer(&t->agent->contact_send_recency, t->contact,
                                                        t->contact_len, (int64_t)time(NULL));
}

/* ── F9 double-text afterthought ─────────────────────────────────────────── */

static void double_text_send(hu_spontaneity_turn_t *t, const hu_provider_vtable_t *dt_vtable,
                             void *dt_ctx, uint32_t dt_seed) {
    struct hu_agent *agent = t->agent;
    hu_allocator_t *alloc = t->alloc;
    const char *response = t->response;
    size_t response_len = t->response_len;
    char dt_user[512];
    int dt_n = snprintf(dt_user, sizeof(dt_user),
                        "You just sent this message: \"%.*s\"\n"
                        "Add a brief, natural follow-up thought (1 short sentence max). "
                        "Something you'd double-text a moment later.",
                        (int)(response_len > 200 ? 200 : response_len), response);
    if (dt_n <= 0 || (size_t)dt_n >= sizeof(dt_user))
        return;
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
            unsigned int dt_delay = 10000u + (dt_seed % 35000u);
            usleep((useconds_t)(dt_delay * 1000u));
            hu_error_t send_err = t->channel->vtable->send(
                t->channel->ctx, t->send_target, t->send_target_len, dt_resp, dt_resp_len, NULL, 0);
            if (send_err == HU_OK && agent->bth_metrics)
                agent->bth_metrics->double_texts++; /* count delivered afterthoughts only */
        }
    }
    if (dt_resp)
        alloc->free(alloc->ctx, dt_resp, dt_resp_len + 1);
}

void hu_daemon_spontaneity_double_text(hu_spontaneity_turn_t *t) {
    if (!t || !t->agent || !t->channel)
        return;
    struct hu_agent *agent = t->agent;
    const hu_provider_vtable_t *dt_vtable =
        t->classify ? t->classify->vtable : agent->provider.vtable;
    void *dt_ctx = t->classify ? t->classify->ctx : agent->provider.ctx;
    bool base = t->response && t->response_len > 0 && agent->persona && t->channel->vtable->send &&
                dt_vtable && dt_vtable->chat_with_system;
    if (!base)
        return;
    uint32_t dt_seed =
        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)t->response;
    hu_gate_mode_t mode = hu_spontaneity_mode();
    if (mode != HU_GATE_LIVE) {
        /* OFF / SHADOW: the original one-emission-per-turn path (unreachable
         * in practice: the reactive send was recorded just before this). */
        if (mode == HU_GATE_SHADOW)
            (void)spontaneity_decide(
                t, HU_SPONT_DOUBLE_TEXT,
                hu_conversation_should_double_text(t->response, t->response_len, t->history,
                                                   t->history_count, t->hour_local, dt_seed, 1.0f),
                mode);
        if (reactive_just_sent(t))
            return;
        float dt_prob = agent->persona->humanization.double_text_probability;
        if (hu_conversation_should_double_text(t->response, t->response_len, t->history,
                                               t->history_count, t->hour_local, dt_seed, dt_prob))
            double_text_send(t, dt_vtable, dt_ctx, dt_seed);
        return;
    }
    bool eligible = hu_conversation_should_double_text(
        t->response, t->response_len, t->history, t->history_count, t->hour_local, dt_seed, 1.0f);
    if (spontaneity_decide(t, HU_SPONT_DOUBLE_TEXT, eligible, mode))
        double_text_send(t, dt_vtable, dt_ctx, dt_seed);
}

/* ── Self-reaction (haha / emphasize our own message) ────────────────────── */

static void self_reaction_send(hu_spontaneity_turn_t *t, hu_reaction_type_t self_r) {
    usleep(1500000 + ((uint32_t)time(NULL) % 3000000));
#ifdef HU_HAS_IMESSAGE
    int64_t sent_id = hu_imessage_get_latest_sent_rowid(t->contact, t->contact_len);
#else
    int64_t sent_id = t->fallback_sent_id;
#endif
    if (sent_id > 0) {
        t->channel->vtable->react(t->channel->ctx, t->send_target, t->send_target_len, sent_id,
                                  self_r);
        hu_log_info("human", t->agent ? t->agent->observer : NULL,
                    "self-reaction on own message: %d", (int)self_r);
    }
}

void hu_daemon_spontaneity_self_reaction(hu_spontaneity_turn_t *t) {
    /* Skip for groups: get_latest_sent_rowid uses handle.id SQL. */
    if (!t || !t->channel || !t->response || t->response_len == 0 || !t->channel->vtable->react ||
        t->is_group)
        return;
    hu_gate_mode_t mode = hu_spontaneity_mode();
    if (mode != HU_GATE_LIVE) {
        if (mode == HU_GATE_SHADOW)
            (void)spontaneity_decide(t, HU_SPONT_SELF_REACTION,
                                     hu_conversation_self_reaction_kind(
                                         t->response, t->response_len) != HU_REACTION_NONE,
                                     mode);
        if (reactive_just_sent(t))
            return;
        hu_reaction_type_t self_r = hu_conversation_classify_self_reaction(
            t->response, t->response_len, (uint32_t)time(NULL));
        if (self_r != HU_REACTION_NONE)
            self_reaction_send(t, self_r);
        return;
    }
    hu_reaction_type_t kind = hu_conversation_self_reaction_kind(t->response, t->response_len);
    if (spontaneity_decide(t, HU_SPONT_SELF_REACTION, kind != HU_REACTION_NONE, mode))
        self_reaction_send(t, kind);
}

/* ── GIF ─────────────────────────────────────────────────────────────────── */

/* GIF rate cap the daemon applied inline: 5 per 10 minutes per contact. */
static bool gif_rate_ok(const hu_spontaneity_turn_t *t, uint64_t now_ms) {
    return hu_conversation_gif_rate_allow(t->contact, t->contact_len, now_ms, 5, 600000);
}

static bool gif_eligible(const hu_spontaneity_turn_t *t, uint64_t now_ms) {
    return hu_conversation_should_send_gif(t->inbound, t->inbound_len, t->history, t->history_count,
                                           0, 1.0f) &&
           gif_rate_ok(t, now_ms);
}

bool hu_daemon_spontaneity_gif_open(hu_spontaneity_turn_t *t, uint64_t now_ms) {
    if (!t)
        return false;
    hu_gate_mode_t mode = hu_spontaneity_mode();
    if (mode == HU_GATE_LIVE)
        return t->fired < 1;
    if (mode == HU_GATE_SHADOW)
        (void)spontaneity_decide(t, HU_SPONT_GIF, gif_eligible(t, now_ms), mode);
    return !reactive_just_sent(t);
}

bool hu_daemon_spontaneity_gif_roll(hu_spontaneity_turn_t *t, uint32_t seed, float legacy_prob,
                                    uint64_t now_ms) {
    if (!t)
        return false;
    hu_gate_mode_t mode = hu_spontaneity_mode();
    if (mode != HU_GATE_LIVE)
        return hu_conversation_should_send_gif(t->inbound, t->inbound_len, t->history,
                                               t->history_count, seed, legacy_prob) &&
               gif_rate_ok(t, now_ms);
    return spontaneity_decide(t, HU_SPONT_GIF, gif_eligible(t, now_ms), mode);
}

#if HU_IS_TEST
void hu_spontaneity_set_for_test(int mode, const hu_spontaneity_rates_t *rates, uint32_t seed) {
    s_mode_override = mode;
    s_rates_override_set = rates != NULL;
    if (rates)
        s_rates_override = *rates;
    s_rng = seed;
}

bool hu_spontaneity_decide_for_test(hu_spontaneity_turn_t *t, hu_spontaneity_kind_t kind,
                                    bool eligible) {
    return spontaneity_decide(t, kind, eligible, hu_spontaneity_mode());
}
#endif
