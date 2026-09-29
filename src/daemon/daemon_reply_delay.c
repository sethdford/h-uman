/*
 * src/daemon/daemon_reply_delay.c — Contract C5, Part C.
 *
 * Loads scripts/fit_reply_delay_model.py's quantile-table JSON and samples
 * a deterministic reply delay from it, plus a SHADOW-mode logging helper
 * that compares the model's prediction against the daemon's own observed
 * delay without touching the send path. See
 * include/human/daemon/reply_delay.h for the full contract.
 */
#include "human/core/paths.h"
#include "human/daemon/reply_delay.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── env gate ─────────────────────────────────────────────────────────── */

hu_reply_delay_mode_t hu_reply_delay_mode_from_env(void) {
    const char *v = getenv("HU_REPLY_DELAY_MODEL");
    if (!v)
        return HU_REPLY_DELAY_MODE_OFF;
    if (strcmp(v, "shadow") == 0)
        return HU_REPLY_DELAY_MODE_SHADOW;
    if (strcmp(v, "live") == 0)
        return HU_REPLY_DELAY_MODE_LIVE;
    /* "off" or any unrecognized value: fail closed. */
    return HU_REPLY_DELAY_MODE_OFF;
}

/* ── quantile extraction ─────────────────────────────────────────────── */

typedef struct reply_delay_quantiles {
    double p10, p25, p50, p75, p90;
    bool ok;
} reply_delay_quantiles_t;

static bool get_number_field(const hu_json_value_t *obj, const char *key, double *out) {
    if (!obj)
        return false;
    hu_json_value_t *v = hu_json_object_get(obj, key);
    if (!v || v->type != HU_JSON_NUMBER)
        return false;
    *out = v->data.number;
    return true;
}

static reply_delay_quantiles_t extract_quantiles(const hu_json_value_t *cell) {
    reply_delay_quantiles_t q;
    memset(&q, 0, sizeof(q));
    if (!cell || cell->type != HU_JSON_OBJECT)
        return q;
    hu_json_value_t *quant = hu_json_object_get(cell, "quantiles");
    if (!quant || quant->type != HU_JSON_OBJECT)
        return q;
    q.ok = get_number_field(quant, "p10", &q.p10) && get_number_field(quant, "p25", &q.p25) &&
           get_number_field(quant, "p50", &q.p50) && get_number_field(quant, "p75", &q.p75) &&
           get_number_field(quant, "p90", &q.p90);
    return q;
}

/* Piecewise-linear inverse-CDF over the five known quantile points.
 * u outside [0.10, 0.90] clamps to the nearest known point rather than
 * extrapolating — a quantile table has no information about the tails
 * beyond p10/p90, and inventing a number there would be exactly the
 * fabrication ~/.claude/rules/no-number-without-a-measurement.md forbids. */
static double interpolate_quantiles(const reply_delay_quantiles_t *q, double u) {
    static const double ps[5] = {0.10, 0.25, 0.50, 0.75, 0.90};
    const double vs[5] = {q->p10, q->p25, q->p50, q->p75, q->p90};
    if (u <= ps[0])
        return vs[0];
    if (u >= ps[4])
        return vs[4];
    for (int i = 0; i < 4; i++) {
        if (u >= ps[i] && u <= ps[i + 1]) {
            double frac = (u - ps[i]) / (ps[i + 1] - ps[i]);
            return vs[i] + frac * (vs[i + 1] - vs[i]);
        }
    }
    return vs[2]; /* unreachable */
}

/* xorshift32 — deterministic per seed, no libc rand() cross-platform
 * variance. seed=0 is remapped to 1 (xorshift's fixed point). */
static uint32_t xorshift32_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static double xorshift32_uniform01(uint32_t *state) {
    /* Top 24 bits -> [0, 2^24) -> [0, 1). */
    return (double)(xorshift32_next(state) >> 8) / (double)(1u << 24);
}

/* ── model file loading ──────────────────────────────────────────────── */

#define HU_REPLY_DELAY_MODEL_MAX_BYTES (8 * 1024 * 1024)

static char *read_file_all(const char *path, size_t *out_len) {
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > HU_REPLY_DELAY_MODEL_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

static const char *length_bucket_name(size_t incoming_len, double lo_chars, double hi_chars) {
    if ((double)incoming_len < lo_chars)
        return "short";
    if ((double)incoming_len <= hi_chars)
        return "medium";
    return "long";
}

static const char *freq_tercile_name(double contact_freq, double lo_count, double hi_count) {
    if (contact_freq <= lo_count)
        return "low";
    if (contact_freq <= hi_count)
        return "mid";
    return "high";
}

int64_t hu_reply_delay_from_model(const char *model_path, int hour, size_t incoming_len,
                                  double contact_freq, uint32_t seed) {
    if (!model_path || !model_path[0])
        return -1;

    size_t buf_len = 0;
    char *buf = read_file_all(model_path, &buf_len);
    if (!buf)
        return -1;

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, buf, buf_len, &root);
    free(buf);
    if (err != HU_OK || !root)
        return -1;
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(&alloc, root);
        return -1;
    }

    double lo_chars = 40.0, hi_chars = 160.0;
    hu_json_value_t *len_thresh = hu_json_object_get(root, "length_bucket_thresholds");
    if (len_thresh) {
        get_number_field(len_thresh, "lo_chars", &lo_chars);
        get_number_field(len_thresh, "hi_chars", &hi_chars);
    }
    double lo_count = 5.0, hi_count = 20.0;
    hu_json_value_t *freq_thresh = hu_json_object_get(root, "freq_tercile_boundaries");
    if (freq_thresh) {
        get_number_field(freq_thresh, "lo_count", &lo_count);
        get_number_field(freq_thresh, "hi_count", &hi_count);
    }

    int h = hour;
    if (h < 0)
        h = 0;
    if (h > 23)
        h = 23;
    const char *lb = length_bucket_name(incoming_len, lo_chars, hi_chars);
    const char *ft = freq_tercile_name(contact_freq, lo_count, hi_count);

    char key_full[64], key_hl[48], key_h[16];
    snprintf(key_full, sizeof(key_full), "h%d_l%s_f%s", h, lb, ft);
    snprintf(key_hl, sizeof(key_hl), "h%d_l%s", h, lb);
    snprintf(key_h, sizeof(key_h), "h%d", h);

    reply_delay_quantiles_t q;
    memset(&q, 0, sizeof(q));

    hu_json_value_t *cells = hu_json_object_get(root, "cells");
    if (cells)
        q = extract_quantiles(hu_json_object_get(cells, key_full));
    if (!q.ok) {
        hu_json_value_t *hl = hu_json_object_get(root, "hour_len_marginals");
        if (hl)
            q = extract_quantiles(hu_json_object_get(hl, key_hl));
    }
    if (!q.ok) {
        hu_json_value_t *hm = hu_json_object_get(root, "hour_marginals");
        if (hm)
            q = extract_quantiles(hu_json_object_get(hm, key_h));
    }
    if (!q.ok)
        q = extract_quantiles(hu_json_object_get(root, "global"));

    if (!q.ok) {
        hu_json_free(&alloc, root);
        return -1;
    }

    uint32_t state = seed ? seed : 1u;
    double u = xorshift32_uniform01(&state);
    double sample = interpolate_quantiles(&q, u);

    hu_json_free(&alloc, root);

    if (sample < 0.0)
        sample = 0.0;
    return (int64_t)(sample + 0.5);
}

/* ── shadow logging ──────────────────────────────────────────────────── */

static const char *hu_reply_delay_default_model_path(char *buf, size_t buf_cap) {
    const char *override = getenv("HU_REPLY_DELAY_MODEL_PATH");
    if (override && override[0]) {
        snprintf(buf, buf_cap, "%s", override);
        return buf;
    }
    hu_paths_state_or(buf, buf_cap, "/tmp", "reply_delay_model.json");
    return buf;
}

/* Small FNV-1a for a privacy-safe contact reference in shadow log lines —
 * never write the raw contact identifier (phone/email) to disk. */
static uint32_t reply_delay_contact_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint32_t)(unsigned char)s[i]) * 16777619u;
    return h;
}

void hu_reply_delay_shadow_log(int hour, size_t incoming_len, double contact_freq,
                               int64_t heuristic_delay_secs, const char *contact_key,
                               size_t contact_key_len, uint32_t seed) {
    if (hu_reply_delay_mode_from_env() != HU_REPLY_DELAY_MODE_SHADOW)
        return;

    char path_buf[512];
    const char *model_path = hu_reply_delay_default_model_path(path_buf, sizeof(path_buf));

    int64_t model_delay_secs =
        hu_reply_delay_from_model(model_path, hour, incoming_len, contact_freq, seed);
    if (model_delay_secs < 0) {
        hu_log_warn("daemon_reply_delay", NULL, "shadow: model unavailable at %s", model_path);
        return;
    }

    uint32_t contact_hash = (contact_key && contact_key_len > 0)
                                ? reply_delay_contact_hash(contact_key, contact_key_len)
                                : 0;
    hu_log_info("daemon_reply_delay", NULL,
                "shadow hour=%d incoming_len=%zu contact_freq=%.2f heuristic_delay_s=%lld "
                "model_delay_s=%lld delta_s=%lld contact_hash=%08x",
                hour, incoming_len, contact_freq, (long long)heuristic_delay_secs,
                (long long)model_delay_secs, (long long)(model_delay_secs - heuristic_delay_secs),
                contact_hash);
}
