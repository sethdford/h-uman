/* src/agent/turn/contact_stage_turn.c — per-contact relationship stage for the
 * turn (DEF-16). Contract in include/human/agent/contact_stage_turn.h. */
#include "human/agent/contact_stage_turn.h"

#include "human/core/json.h"
#include "human/core/log.h"
#include "human/persona.h"
#include "human/persona/card_file.h"
#include "human/persona/contact_stage.h"

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/contact_stage_repo.h"
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

hu_gate_mode_t hu_contact_stage_mode(void) {
    return hu_gate_mode_from_env("HU_REL_STAGE_DERIVED", HU_GATE_OFF);
}

static float cs_prior(const hu_agent_t *agent, const char *contact, size_t len) {
    const hu_contact_profile_t *cp =
        agent->persona ? hu_persona_find_contact(agent->persona, contact, len) : NULL;
    return cp ? hu_contact_stage_prior(cp->dunbar_layer, cp->relationship_stage) : -1.0f;
}

static const char *mode_name(hu_gate_mode_t m) {
    return m == HU_GATE_LIVE ? "live" : "shadow";
}

static void cs_apply(hu_agent_t *agent, hu_gate_mode_t mode, hu_relationship_stage_t st,
                     bool failed, float q, float prior, const hu_contact_stage_signals_t *sig) {
    hu_relationship_stage_t prev = agent->relationship.stage;
    hu_log_info("contact_stage", NULL,
                "[contact_stage %s] prev=%d stage=%d failed=%d q=%.2f prior=%d inbound=%u days=%u "
                "seth_replies=%lld",
                mode_name(mode), (int)prev, (int)st, failed ? 1 : 0, (double)q,
                prior >= 0.0f ? (int)hu_relationship_stage_from_quality(prior) : -1, sig->inbound,
                sig->active_days, (long long)sig->seth_replies);
    if (mode != HU_GATE_LIVE)
        return;
    agent->relationship.stage = st;
    agent->relationship.derived = true;
}

#ifdef HU_ENABLE_SQLITE

#define CS_MAX_CONTACTS 2048u

/* ── owner-level norms + Seth's reply counts, cached ─────────────────────── */

typedef struct {
    uint64_t hash;
    int64_t replies;
} cs_reply_t;

static struct {
    bool valid;
    const void *db;
    uint64_t persona_hash;
    int64_t computed_at;
    hu_contact_stage_norms_t norms;
    cs_reply_t replies[CS_MAX_CONTACTS];
    size_t replies_n;
} s_cache;
static pthread_mutex_t s_cache_mu = PTHREAD_MUTEX_INITIALIZER;

void hu_contact_stage_cache_reset(void) {
    pthread_mutex_lock(&s_cache_mu);
    s_cache.valid = false;
    pthread_mutex_unlock(&s_cache_mu);
}

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; s && i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Seth's own reply turns per contact from <persona>.learned-style.json
 * (contacts.<handle>.overall.n). Absent file -> none known. */
static size_t load_replies(hu_allocator_t *alloc, const char *persona, size_t persona_len,
                           cs_reply_t *out, size_t cap) {
    if (!alloc || !persona || persona_len == 0)
        return 0;
    char *buf = NULL;
    size_t len = 0;
    if (hu_persona_card_slurp(alloc, persona, persona_len, ".learned-style.json", &buf, &len) !=
        HU_OK)
        return 0;
    hu_json_value_t *root = NULL;
    size_t n = 0;
    if (hu_persona_card_parse_object(alloc, buf, len, &root) == HU_OK) {
        hu_json_value_t *contacts = hu_json_object_get(root, "contacts");
        if (contacts && contacts->type == HU_JSON_OBJECT) {
            for (size_t i = 0; i < contacts->data.object.len && n < cap; i++) {
                const hu_json_pair_t *p = &contacts->data.object.pairs[i];
                hu_json_value_t *overall = hu_json_object_get(p->value, "overall");
                double v = overall ? hu_json_get_number(overall, "n", -1.0) : -1.0;
                if (v < 0.0 || !p->key)
                    continue;
                out[n].hash = fnv(p->key, p->key_len);
                out[n].replies = (int64_t)v;
                n++;
            }
        }
        hu_json_free(alloc, root);
    }
    alloc->free(alloc->ctx, buf, len + 1);
    return n;
}

static int64_t replies_for(const char *contact, size_t len) {
    uint64_t h = fnv(contact, len);
    for (size_t i = 0; i < s_cache.replies_n; i++)
        if (s_cache.replies[i].hash == h)
            return s_cache.replies[i].replies;
    return -1;
}

typedef struct {
    const hu_agent_t *agent;
    double inbound[CS_MAX_CONTACTS];
    double days[CS_MAX_CONTACTS];
    double ratio[CS_MAX_CONTACTS];
    size_t n, n_ratio;
    /* second pass: the after-distribution */
    hu_contact_stage_norms_t norms;
    uint32_t after[4];
} cs_scan_t;

static void cs_collect(void *ctx, const char *contact, size_t len, uint32_t inbound,
                       uint32_t active_days) {
    cs_scan_t *s = (cs_scan_t *)ctx;
    if (s->n >= CS_MAX_CONTACTS)
        return;
    s->inbound[s->n] = (double)inbound;
    s->days[s->n] = (double)active_days;
    s->n++;
    int64_t r = replies_for(contact, len);
    if (r >= 0 && inbound > 0)
        s->ratio[s->n_ratio++] = (double)r / (double)inbound;
}

static void cs_count(void *ctx, const char *contact, size_t len, uint32_t inbound,
                     uint32_t active_days) {
    cs_scan_t *s = (cs_scan_t *)ctx;
    hu_contact_stage_signals_t sig = {inbound, active_days, replies_for(contact, len)};
    s->after[(int)hu_contact_stage_derive(&sig, &s->norms, cs_prior(s->agent, contact, len),
                                          NULL)]++;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, size_t n) {
    if (n == 0)
        return 0.0;
    qsort(v, n, sizeof(double), cmp_double);
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* Recompute the cache (caller holds s_cache_mu). */
static void cs_recompute(sqlite3 *db, const hu_agent_t *agent, uint64_t persona_hash, int64_t now) {
    s_cache.replies_n = load_replies(agent->alloc, agent->persona_name, agent->persona_name_len,
                                     s_cache.replies, CS_MAX_CONTACTS);
    cs_scan_t *scan = (cs_scan_t *)calloc(1, sizeof(cs_scan_t));
    hu_contact_stage_norms_t norms = {HU_CONTACT_STAGE_DEFAULT_MEDIAN_INBOUND,
                                      HU_CONTACT_STAGE_DEFAULT_MEDIAN_DAYS, 0.0};
    if (scan) {
        scan->agent = agent;
        if (hu_contact_stage_repo_each_contact(db, cs_collect, scan) == HU_OK &&
            scan->n >= HU_CONTACT_STAGE_MIN_NORM_CONTACTS) {
            norms.median_inbound = median(scan->inbound, scan->n);
            norms.median_days = median(scan->days, scan->n);
        }
        norms.median_reply_ratio = median(scan->ratio, scan->n_ratio);
        scan->norms = norms;
        (void)hu_contact_stage_repo_each_contact(db, cs_count, scan);
        uint32_t before[4] = {0};
        (void)hu_contact_stage_repo_persisted_counts(db, before);
        hu_log_info("contact_stage", NULL,
                    "distribution contacts=%zu before=%u/%u/%u/%u after=%u/%u/%u/%u "
                    "median_inbound=%.0f median_days=%.0f reply_ratio=%.2f profile_contacts=%zu",
                    scan->n, before[0], before[1], before[2], before[3], scan->after[0],
                    scan->after[1], scan->after[2], scan->after[3], norms.median_inbound,
                    norms.median_days, norms.median_reply_ratio, s_cache.replies_n);
        free(scan);
    }
    s_cache.norms = norms;
    s_cache.db = db;
    s_cache.persona_hash = persona_hash;
    s_cache.computed_at = now;
    s_cache.valid = true;
}

hu_relationship_stage_t hu_contact_stage_refresh_at(hu_agent_t *agent, const char *contact,
                                                    size_t contact_len, int64_t now) {
    if (!agent)
        return HU_REL_NEW;
    hu_gate_mode_t mode = hu_contact_stage_mode();
    if (mode == HU_GATE_OFF)
        return agent->relationship.stage;
    if (!contact || contact_len == 0) {
        if (mode == HU_GATE_LIVE)
            agent->relationship.derived = false; /* contactless (CLI): old fallback */
        return agent->relationship.stage;
    }
    float prior = cs_prior(agent, contact, contact_len);
    hu_contact_stage_signals_t sig = {0, 0, -1};
    sqlite3 *db = agent->memory ? hu_sqlite_memory_get_db(agent->memory) : NULL;
    if (!db || hu_contact_stage_repo_contact_counts(db, contact, contact_len, &sig.inbound,
                                                    &sig.active_days) != HU_OK) {
        /* Never keep the previous contact's stage: the prior alone. */
        hu_contact_stage_norms_t dflt = {HU_CONTACT_STAGE_DEFAULT_MEDIAN_INBOUND,
                                         HU_CONTACT_STAGE_DEFAULT_MEDIAN_DAYS, 0.0};
        hu_contact_stage_signals_t none = {0, 0, -1};
        float q = 0.0f;
        hu_relationship_stage_t st = hu_contact_stage_derive(&none, &dflt, prior, &q);
        cs_apply(agent, mode, st, true, q, prior, &none);
        return agent->relationship.stage;
    }
    uint64_t ph = fnv(agent->persona_name, agent->persona_name_len);
    pthread_mutex_lock(&s_cache_mu);
    if (!s_cache.valid || s_cache.db != db || s_cache.persona_hash != ph ||
        now - s_cache.computed_at >= HU_CONTACT_STAGE_NORMS_TTL_S || now < s_cache.computed_at)
        cs_recompute(db, agent, ph, now);
    hu_contact_stage_norms_t norms = s_cache.norms;
    sig.seth_replies = replies_for(contact, contact_len);
    pthread_mutex_unlock(&s_cache_mu);

    float q = 0.0f;
    hu_relationship_stage_t st = hu_contact_stage_derive(&sig, &norms, prior, &q);
    cs_apply(agent, mode, st, false, q, prior, &sig);
    if (mode == HU_GATE_LIVE)
        (void)hu_contact_stage_repo_save_derived(db, contact, contact_len, (int)st, (double)q,
                                                 sig.inbound, sig.active_days, sig.seth_replies,
                                                 now);
    return agent->relationship.stage;
}

#else /* !HU_ENABLE_SQLITE */

void hu_contact_stage_cache_reset(void) {}

hu_relationship_stage_t hu_contact_stage_refresh_at(hu_agent_t *agent, const char *contact,
                                                    size_t contact_len, int64_t now) {
    (void)now;
    if (!agent)
        return HU_REL_NEW;
    hu_gate_mode_t mode = hu_contact_stage_mode();
    if (mode == HU_GATE_OFF || !contact || contact_len == 0)
        return agent->relationship.stage;
    /* No session store: the prior alone, never the previous contact's stage. */
    float prior = cs_prior(agent, contact, contact_len);
    hu_contact_stage_norms_t dflt = {HU_CONTACT_STAGE_DEFAULT_MEDIAN_INBOUND,
                                     HU_CONTACT_STAGE_DEFAULT_MEDIAN_DAYS, 0.0};
    hu_contact_stage_signals_t none = {0, 0, -1};
    float q = 0.0f;
    cs_apply(agent, mode, hu_contact_stage_derive(&none, &dflt, prior, &q), true, q, prior, &none);
    return agent->relationship.stage;
}

#endif /* HU_ENABLE_SQLITE */

hu_relationship_stage_t hu_contact_stage_refresh(hu_agent_t *agent, const char *contact,
                                                 size_t contact_len) {
    return hu_contact_stage_refresh_at(agent, contact, contact_len, (int64_t)time(NULL));
}
