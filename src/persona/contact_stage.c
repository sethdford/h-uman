/* contact_stage.c — per-contact relationship stage from interaction data
 * (DEF-16). Formula and rationale in include/human/persona/contact_stage.h. */
#include "human/persona/contact_stage.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Midpoint of each stage band (hu_relationship_stage_from_quality). */
#define PRIOR_DEEP     0.90f
#define PRIOR_TRUSTED  0.675f
#define PRIOR_FAMILIAR 0.40f
#define PRIOR_NEW      0.125f

static bool word_eq(const char *s, const char *w) {
    return s && strcasecmp(s, w) == 0;
}

static float prior_from_layer(const char *v) {
    if (!v || !v[0])
        return -1.0f;
    /* Skip surrounding whitespace by comparing a trimmed copy. */
    char buf[32];
    size_t n = 0;
    while (*v && isspace((unsigned char)*v))
        v++;
    while (v[n] && n + 1 < sizeof(buf)) {
        buf[n] = v[n];
        n++;
    }
    while (n > 0 && isspace((unsigned char)buf[n - 1]))
        n--;
    buf[n] = '\0';
    if (word_eq(buf, "intimate") || word_eq(buf, "support") || word_eq(buf, "1") ||
        word_eq(buf, "5"))
        return PRIOR_DEEP;
    if (word_eq(buf, "close") || word_eq(buf, "sympathy") || word_eq(buf, "2") ||
        word_eq(buf, "15"))
        return PRIOR_TRUSTED;
    if (word_eq(buf, "active") || word_eq(buf, "affinity") || word_eq(buf, "3") ||
        word_eq(buf, "50"))
        return PRIOR_FAMILIAR;
    if (word_eq(buf, "acquaintance") || word_eq(buf, "casual") || word_eq(buf, "4") ||
        word_eq(buf, "150"))
        return PRIOR_NEW;
    return -1.0f;
}

float hu_contact_stage_prior(const char *dunbar_layer, const char *relationship_stage) {
    float p = prior_from_layer(dunbar_layer);
    return p >= 0.0f ? p : prior_from_layer(relationship_stage);
}

static double saturate(double x, double half) {
    if (x <= 0.0)
        return 0.0;
    if (half <= 0.0)
        return 1.0;
    return 1.0 - pow(2.0, -x / half);
}

float hu_contact_stage_evidence(const hu_contact_stage_signals_t *s,
                                const hu_contact_stage_norms_t *norms) {
    if (!s || !norms)
        return 0.0f;
    double n = (double)s->inbound + (double)s->outbound;
    if (n <= 0.0)
        return 0.0f;
    double vol = saturate(n, norms->median_msgs);
    double reg = saturate((double)s->active_days, norms->median_days);
    double lo = s->inbound < s->outbound ? (double)s->inbound : (double)s->outbound;
    double recip = 2.0 * lo / n;
    return (float)((vol + reg) / 2.0 * (0.75 + 0.25 * recip));
}

hu_relationship_stage_t hu_contact_stage_derive(const hu_contact_stage_signals_t *s,
                                                const hu_contact_stage_norms_t *norms, float prior,
                                                float *quality_out) {
    if (quality_out)
        *quality_out = 0.0f;
    if (!s || !norms)
        return HU_REL_NEW;
    float e = hu_contact_stage_evidence(s, norms);
    float q = e;
    if (prior >= 0.0f) {
        double n = (double)s->inbound + (double)s->outbound;
        double half = norms->median_msgs > 0.0 ? norms->median_msgs : 1.0;
        float w = (float)(n / (n + half));
        q = (1.0f - w) * prior + w * e;
    }
    if (quality_out)
        *quality_out = q;
    return hu_relationship_stage_from_quality(q);
}
