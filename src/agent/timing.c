/*
 * Statistical Timing Model — extracts real response time distributions from
 * chat.db and samples from them. Replaces hand-configured delay ranges with
 * learned distributions from the persona owner's actual iMessage history.
 */
#include "human/agent/timing.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define HU_TIMING_MIN_SAMPLES 5
#define MS_PER_SEC 1000
#define LCG_MUL 1103515245u
#define LCG_ADD 12345u

static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db)
        return -1;
    if (da > db)
        return 1;
    return 0;
}

static size_t msg_length_bucket(size_t len) {
    if (len < 20)
        return 0;
    if (len <= 100)
        return 1;
    return 2;
}

static uint32_t lcg_next(uint32_t *state) {
    *state = *state * LCG_MUL + LCG_ADD;
    return *state;
}

static size_t perc_idx(size_t n, unsigned p) {
    size_t x = (size_t)((double)n * (double)p / 100.0 + 0.999999);
    if (x == 0)
        return 0;
    return x - 1 < n ? x - 1 : n - 1;
}

hu_error_t hu_timing_model_build_query(const char *contact_id, size_t contact_id_len, char *buf,
                                       size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    static const char BASE[] =
        "SELECT "
        "h.id AS handle, "
        "CAST(strftime('%H', datetime(m1.date/1000000000 + 978307200, 'unixepoch', 'localtime')) "
        "AS INTEGER) AS hour, "
        "CAST(strftime('%w', datetime(m1.date/1000000000 + 978307200, 'unixepoch', 'localtime')) "
        "AS INTEGER) AS dow, "
        "length(m1.text) AS msg_len, "
        "(m2.date - m1.date) / 1000000000.0 AS response_sec "
        "FROM message m1 "
        "JOIN message m2 ON m2.ROWID = ("
        "SELECT MIN(m3.ROWID) FROM message m3 "
        "WHERE m3.handle_id = m1.handle_id "
        "AND m3.is_from_me = 1 "
        "AND m3.ROWID > m1.ROWID "
        "AND m3.date > m1.date "
        "AND (m3.date - m1.date) / 1000000000.0 < 86400 "
        "AND (m3.date - m1.date) / 1000000000.0 > 2"
        ") "
        "JOIN handle h ON h.ROWID = m1.handle_id "
        "WHERE m1.is_from_me = 0 "
        "AND m1.text IS NOT NULL";

    size_t pos = 0;
    size_t base_len = sizeof(BASE) - 1;
    if (pos + base_len + 1 > cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, BASE, base_len);
    pos = base_len;

    if (contact_id && contact_id_len > 0) {
        /* Sanitize contact_id: escape single quotes by doubling them */
        char sanitized[257];
        size_t out_pos = 0;
        for (size_t i = 0; i < contact_id_len && out_pos < 256; i++) {
            if (contact_id[i] == '\'') {
                if (out_pos + 2 > 256)
                    break;
                sanitized[out_pos++] = '\'';
                sanitized[out_pos++] = '\'';
            } else {
                if (out_pos + 1 > 256)
                    break;
                sanitized[out_pos++] = contact_id[i];
            }
        }
        sanitized[out_pos] = '\0';

        /* Add AND h.id = '...' */
        int n = snprintf(buf + pos, cap - pos, " AND h.id = '%s'", sanitized);
        if (n < 0 || pos + (size_t)n >= cap)
            return HU_ERR_INVALID_ARGUMENT;
        pos += (size_t)n;
    }

    buf[pos] = '\0';
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_timing_bucket_from_samples(const double *samples, size_t count,
                                         hu_timing_bucket_t *out) {
    if (!samples || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (count < HU_TIMING_MIN_SAMPLES)
        return HU_ERR_INVALID_ARGUMENT;

    double *sorted = (double *)malloc(count * sizeof(double));
    if (!sorted)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(sorted, samples, count * sizeof(double));
    qsort(sorted, count, sizeof(double), compare_double);

    double sum = 0.0;
    for (size_t i = 0; i < count; i++)
        sum += sorted[i];

    /* Percentile indices: nearest-rank method */
    size_t i10 = perc_idx(count, 10);
    size_t i25 = perc_idx(count, 25);
    size_t i50 = perc_idx(count, 50);
    size_t i75 = perc_idx(count, 75);
    size_t i90 = perc_idx(count, 90);
    out->p10 = sorted[i10];
    out->p25 = sorted[i25];
    out->p50 = sorted[i50];
    out->p75 = sorted[i75];
    out->p90 = sorted[i90];
    out->mean = sum / (double)count;
    out->sample_count = (uint32_t)count;

    free(sorted);
    return HU_OK;
}

static void blend_buckets(const hu_timing_bucket_t *a, const hu_timing_bucket_t *b,
                         hu_timing_bucket_t *out) {
    if (a->sample_count == 0 && b->sample_count == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }
    if (a->sample_count == 0) {
        *out = *b;
        return;
    }
    if (b->sample_count == 0) {
        *out = *a;
        return;
    }
    out->p10 = (a->p10 + b->p10) / 2.0;
    out->p25 = (a->p25 + b->p25) / 2.0;
    out->p50 = (a->p50 + b->p50) / 2.0;
    out->p75 = (a->p75 + b->p75) / 2.0;
    out->p90 = (a->p90 + b->p90) / 2.0;
    out->mean = (a->mean + b->mean) / 2.0;
    out->sample_count = a->sample_count + b->sample_count;
}

static uint64_t sample_from_bucket(const hu_timing_bucket_t *bucket, uint32_t *seed) {
    double t = (double)(lcg_next(seed) >> 16) / 65536.0; /* [0, 1) */
    double val = bucket->p10 + t * (bucket->p90 - bucket->p10);
    if (val < bucket->p10)
        val = bucket->p10;
    if (val > bucket->p90)
        val = bucket->p90;
    return (uint64_t)(val * MS_PER_SEC);
}

uint64_t hu_timing_model_sample(const hu_timing_model_t *model, uint8_t hour, uint8_t day_of_week,
                                size_t incoming_msg_len, uint32_t seed) {
    if (!model)
        return hu_timing_model_sample_default(hour, seed);

    hu_timing_bucket_t blended;
    blend_buckets(&model->by_hour[hour], &model->by_day[day_of_week], &blended);

    hu_timing_bucket_t effective;
    memset(&effective, 0, sizeof(effective));

    if (blended.sample_count > 0) {
        size_t len_idx = msg_length_bucket(incoming_msg_len);
        if (model->by_msg_length[len_idx].sample_count > 0)
            blend_buckets(&blended, &model->by_msg_length[len_idx], &effective);
        else
            effective = blended;
    } else {
        size_t len_idx = msg_length_bucket(incoming_msg_len);
        if (model->by_msg_length[len_idx].sample_count > 0)
            effective = model->by_msg_length[len_idx];
        else if (model->overall.sample_count > 0)
            effective = model->overall;
    }

    if (effective.sample_count == 0 && model->overall.sample_count > 0)
        effective = model->overall;

    if (effective.sample_count == 0)
        return hu_timing_model_sample_default(hour, seed);

    uint32_t s = seed;
    lcg_next(&s);
    return sample_from_bucket(&effective, &s);
}

uint64_t hu_timing_model_sample_default(uint8_t hour, uint32_t seed) {
    double low_sec;
    double high_sec;
    double median_sec;

    if (hour >= 8 && hour <= 22) {
        low_sec = 15.0;
        high_sec = 120.0;
        median_sec = 45.0;
    } else if (hour >= 22 || hour <= 1) {
        low_sec = 60.0;
        high_sec = 600.0;
        median_sec = 180.0;
    } else {
        low_sec = 300.0;
        high_sec = 3600.0;
        median_sec = 1200.0;
    }

    uint32_t s = seed;
    lcg_next(&s);
    double t = (double)(lcg_next(&s) >> 16) / 65536.0;
    double u = (double)(lcg_next(&s) >> 16) / 65536.0;
    /* Favor median: blend uniform draw toward median (70% uniform, 30% median) */
    double blend = 0.7 * ((t + u) / 2.0) + 0.3 * ((median_sec - low_sec) / (high_sec - low_sec));
    double val = low_sec + blend * (high_sec - low_sec);
    if (val < low_sec)
        val = low_sec;
    if (val > high_sec)
        val = high_sec;
    return (uint64_t)(val * MS_PER_SEC);
}

uint64_t hu_timing_adjust(uint64_t base_delay_ms, uint8_t conversation_depth,
                          double emotional_intensity, bool calendar_busy,
                          uint64_t their_last_response_ms) {
    double factor = 1.0;

    if (conversation_depth > 5)
        factor *= 0.7;
    if (emotional_intensity > 0.7)
        factor *= 1.3;
    if (calendar_busy)
        factor *= 2.5;
    if (their_last_response_ms > 0 && their_last_response_ms < base_delay_ms)
        factor *= 0.8;

    double result = (double)base_delay_ms * factor;
    if (result < 0.0)
        result = 0.0;
    return (uint64_t)result;
}

void hu_timing_model_deinit(hu_allocator_t *alloc, hu_timing_model_t *model) {
    if (!alloc || !model)
        return;
    if (model->contact_id) {
        alloc->free(alloc->ctx, model->contact_id, model->contact_id_len + 1);
        model->contact_id = NULL;
        model->contact_id_len = 0;
    }
}
