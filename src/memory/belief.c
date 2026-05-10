#include "human/memory/belief.h"

#include <math.h>
#include <string.h>

/* Clamp helper */
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Safe string copy that always NUL-terminates. */
static void safe_strncpy(char *dst, const char *src, size_t n) {
    if (!src || n == 0)
        return;
    size_t i;
    for (i = 0; i + 1 < n && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

hu_belief_t hu_belief_init(float mean, const char *source, int64_t now) {
    hu_belief_t b;
    memset(&b, 0, sizeof(b));
    b.mean = clampf(mean, 0.0f, 1.0f);
    /* Beta(1,1) with one observation: variance = mean*(1-mean). */
    b.variance = b.mean * (1.0f - b.mean);
    b.last_updated = now;
    b.prov_count = 1;
    safe_strncpy(b.prov[0].source, source ? source : "", sizeof(b.prov[0].source));
    b.prov[0].observed_at = now;
    b.prov[0].weight = 1.0f;
    return b;
}

hu_belief_t hu_belief_update(const hu_belief_t *prior, float observation,
                              const char *source, int64_t now) {
    if (!prior) {
        return hu_belief_init(observation, source, now);
    }

    observation = clampf(observation, 0.0f, 1.0f);

    hu_belief_t b = *prior;
    b.last_updated = now;

    float diff = observation - prior->mean;
    float abs_diff = diff < 0.0f ? -diff : diff;

    /* Effective observation count approximated from variance:
     * For Beta(a,b): variance = ab / ((a+b)^2*(a+b+1)).
     * We track n_eff = 1/variance as a precision proxy.
     * On each update we increment n_eff by 1. */
    float precision = (prior->variance > 1e-9f) ? (1.0f / prior->variance) : 1e6f;
    float n_eff = precision; /* current effective count */

    /* New mean: running average with weight proportional to n_eff. */
    float new_mean = (prior->mean * n_eff + observation) / (n_eff + 1.0f);
    new_mean = clampf(new_mean, 0.0f, 1.0f);

    /* Variance update:
     * Corroborating (|diff| < 0.5): variance shrinks — new obs agrees, precision grows.
     * Contradicting (|diff| >= 0.5): variance grows — uncertainty increases. */
    float new_variance;
    if (abs_diff < 0.5f) {
        /* Shrink: new precision = n_eff + 1; new variance = 1/new_precision. */
        float new_precision = n_eff + 1.0f;
        new_variance = 1.0f / new_precision;
    } else {
        /* Grow: contradicting obs add noise proportional to disagreement. */
        new_variance = prior->variance + abs_diff * abs_diff * (1.0f / (n_eff + 1.0f));
    }
    /* Clamp: variance lives in [0, 0.25] (max for a [0,1] variable). */
    b.variance = clampf(new_variance, 0.0f, 0.25f);
    b.mean = new_mean;

    /* Append provenance (ring buffer, wrapping at 4). */
    uint8_t slot = b.prov_count < 4 ? b.prov_count : (uint8_t)3;
    if (b.prov_count >= 4) {
        /* Shift left to make room at slot 3. */
        for (int i = 0; i < 3; i++)
            b.prov[i] = b.prov[i + 1];
    }
    safe_strncpy(b.prov[slot].source, source ? source : "", sizeof(b.prov[slot].source));
    b.prov[slot].observed_at = now;
    b.prov[slot].weight = 1.0f / (n_eff + 1.0f);
    if (b.prov_count < 4)
        b.prov_count++;

    return b;
}

hu_belief_t hu_belief_combine(const hu_belief_t *a, const hu_belief_t *b) {
    if (!a && !b) {
        hu_belief_t z;
        memset(&z, 0, sizeof(z));
        return z;
    }
    if (!a) return *b;
    if (!b) return *a;

    hu_belief_t out;
    memset(&out, 0, sizeof(out));

    /* Inverse-variance pooling (precision weighting). */
    if (a->variance < 1e-9f && b->variance < 1e-9f) {
        /* Both degenerate: simple average. */
        out.mean = (a->mean + b->mean) * 0.5f;
        out.variance = 0.0f;
    } else if (a->variance < 1e-9f) {
        out.mean = a->mean;
        out.variance = 0.0f;
    } else if (b->variance < 1e-9f) {
        out.mean = b->mean;
        out.variance = 0.0f;
    } else {
        float pa = 1.0f / a->variance;
        float pb = 1.0f / b->variance;
        float pt = pa + pb;
        out.mean = clampf((pa * a->mean + pb * b->mean) / pt, 0.0f, 1.0f);
        out.variance = clampf(1.0f / pt, 0.0f, 0.25f);
    }

    /* Merge provenance: take up to 4 across both. */
    uint8_t n = 0;
    for (uint8_t i = 0; i < a->prov_count && n < 4; i++, n++)
        out.prov[n] = a->prov[i];
    for (uint8_t i = 0; i < b->prov_count && n < 4; i++, n++)
        out.prov[n] = b->prov[i];
    out.prov_count = n;

    out.last_updated = a->last_updated > b->last_updated ? a->last_updated : b->last_updated;
    return out;
}

bool hu_belief_significantly_disagrees(const hu_belief_t *a, const hu_belief_t *b,
                                        float sigma_threshold) {
    if (!a || !b)
        return false;
    float diff = a->mean - b->mean;
    if (diff < 0.0f) diff = -diff;
    /* Combined spread: sqrt(var_a + var_b). */
    float spread = sqrtf(a->variance + b->variance);
    if (spread < 1e-9f)
        return diff > 1e-6f;
    return diff > sigma_threshold * spread;
}
