/* src/agent/turn/contact_stage_turn.c — per-contact relationship stage for the
 * turn (DEF-16). Contract in include/human/agent/contact_stage_turn.h. */
#include "human/agent/contact_stage_turn.h"

#include "human/core/log.h"
#include "human/persona.h"
#include "human/persona/contact_stage.h"

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/contact_stage_repo.h"
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

#define CS_MAX_CONTACTS 4096u

typedef struct {
    const char *want; /* the turn's contact */
    size_t want_len;
    hu_contact_stage_signals_t self;
    bool found;
    double msgs[CS_MAX_CONTACTS];
    double days[CS_MAX_CONTACTS];
    size_t n;
} cs_scan_t;

static void cs_collect(void *ctx, const char *contact, size_t contact_len, uint32_t inbound,
                       uint32_t outbound, uint32_t active_days) {
    cs_scan_t *s = (cs_scan_t *)ctx;
    if (s->want && contact_len == s->want_len && memcmp(contact, s->want, contact_len) == 0) {
        s->self.inbound = inbound;
        s->self.outbound = outbound;
        s->self.active_days = active_days;
        s->found = true;
    }
    if (s->n < CS_MAX_CONTACTS) {
        s->msgs[s->n] = (double)inbound + (double)outbound;
        s->days[s->n] = (double)active_days;
        s->n++;
    }
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, size_t n) {
    qsort(v, n, sizeof(double), cmp_double);
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static hu_contact_stage_norms_t cs_norms(cs_scan_t *s) {
    hu_contact_stage_norms_t norms = {HU_CONTACT_STAGE_DEFAULT_MEDIAN_MSGS,
                                      HU_CONTACT_STAGE_DEFAULT_MEDIAN_DAYS};
    if (s->n >= HU_CONTACT_STAGE_MIN_NORM_CONTACTS) {
        norms.median_msgs = median(s->msgs, s->n);
        norms.median_days = median(s->days, s->n);
    }
    return norms;
}

static float cs_prior(const hu_agent_t *agent, const char *contact, size_t len) {
    const hu_contact_profile_t *cp =
        agent->persona ? hu_persona_find_contact(agent->persona, contact, len) : NULL;
    return cp ? hu_contact_stage_prior(cp->dunbar_layer, cp->relationship_stage) : -1.0f;
}

typedef struct {
    const hu_agent_t *agent;
    hu_contact_stage_norms_t norms;
    uint32_t after[4];
    uint32_t contacts;
} cs_dist_t;

static void cs_count(void *ctx, const char *contact, size_t contact_len, uint32_t inbound,
                     uint32_t outbound, uint32_t active_days) {
    cs_dist_t *d = (cs_dist_t *)ctx;
    hu_contact_stage_signals_t sig = {inbound, outbound, active_days};
    hu_relationship_stage_t st =
        hu_contact_stage_derive(&sig, &d->norms, cs_prior(d->agent, contact, contact_len), NULL);
    d->after[(int)st]++;
    d->contacts++;
}

/* Once per process, before the first save: what was persisted (the old
 * agent-wide counter's snapshots) vs what each contact derives to now. */
static void cs_log_distribution_once(sqlite3 *db, const hu_agent_t *agent,
                                     hu_contact_stage_norms_t norms) {
    static bool logged = false;
    if (logged)
        return;
    logged = true;
    uint32_t before[4] = {0};
    (void)hu_contact_stage_repo_persisted_counts(db, before);
    cs_dist_t d;
    memset(&d, 0, sizeof(d));
    d.agent = agent;
    d.norms = norms;
    (void)hu_contact_stage_repo_each_contact(db, cs_count, &d);
    hu_log_info("contact_stage", NULL,
                "distribution contacts=%u before=%u/%u/%u/%u after=%u/%u/%u/%u "
                "median_msgs=%.0f median_days=%.0f",
                d.contacts, before[0], before[1], before[2], before[3], d.after[0], d.after[1],
                d.after[2], d.after[3], norms.median_msgs, norms.median_days);
}

hu_relationship_stage_t hu_contact_stage_refresh(hu_agent_t *agent, const char *contact,
                                                 size_t contact_len) {
    if (!agent)
        return HU_REL_NEW;
    if (!contact || contact_len == 0 || !agent->memory)
        return agent->relationship.stage;
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return agent->relationship.stage;
    cs_scan_t *scan = (cs_scan_t *)calloc(1, sizeof(cs_scan_t));
    if (!scan)
        return agent->relationship.stage;
    scan->want = contact;
    scan->want_len = contact_len;
    if (hu_contact_stage_repo_each_contact(db, cs_collect, scan) != HU_OK) {
        free(scan);
        return agent->relationship.stage;
    }
    hu_contact_stage_signals_t self = scan->self; /* zeros when the contact is new */
    hu_contact_stage_norms_t norms = cs_norms(scan);
    free(scan);

    cs_log_distribution_once(db, agent, norms);
    float prior = cs_prior(agent, contact, contact_len);
    float q = 0.0f;
    hu_relationship_stage_t prev = agent->relationship.stage;
    hu_relationship_stage_t st = hu_contact_stage_derive(&self, &norms, prior, &q);
    agent->relationship.stage = st;
    /* Active days with this contact; today's conversation counts as one. */
    agent->relationship.session_count = self.active_days > 0 ? self.active_days : 1;
    agent->relationship.total_turns = self.inbound + self.outbound;
    agent->relationship.derived = true;
    /* Existing rows migrate to the derived stage now; a new contact's row is
     * written by the end-of-turn frontier save from agent->relationship. */
    (void)hu_contact_stage_repo_update_persisted(db, contact, contact_len, (int)st,
                                                 (int)agent->relationship.session_count,
                                                 (int)(self.inbound + self.outbound));
    hu_log_info("contact_stage", NULL, "stage=%d prev=%d q=%.2f prior=%d msgs=%u days=%u", (int)st,
                (int)prev, (double)q,
                prior >= 0.0f ? (int)hu_relationship_stage_from_quality(prior) : -1,
                self.inbound + self.outbound, self.active_days);
    return st;
}

#else /* !HU_ENABLE_SQLITE */

hu_relationship_stage_t hu_contact_stage_refresh(hu_agent_t *agent, const char *contact,
                                                 size_t contact_len) {
    (void)contact;
    (void)contact_len;
    return agent ? agent->relationship.stage : HU_REL_NEW;
}

#endif /* HU_ENABLE_SQLITE */
