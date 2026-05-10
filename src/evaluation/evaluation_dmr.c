/* W16 — DMR (Deep Memory Retrieval) backend.
 *
 * Real DMR scores recall@K over a vector store with thousands of documents.
 * Real fetch is gated; this file ships a tiny synthetic 8-dim index of 20
 * vectors with five labelled query→target pairs and reports recall@1,
 * recall@5, recall@10. The math is identical to the production path so the
 * regression gate's recall@10 threshold (3pt drop) can be exercised offline.
 */

#include "human/evaluation/evaluation.h"
#include "evaluation_internal.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DMR_DIM 8
#define DMR_INDEX_N 20
#define DMR_QUERY_N 5

typedef struct {
    int64_t id;
    float vec[DMR_DIM];
} dmr_doc_t;

typedef struct {
    float query[DMR_DIM];
    int64_t target_id;
} dmr_query_t;

/* Deterministic 8-dim vectors. The first 5 are crafted so each is the closest
 * match to the corresponding query below; the rest are noise. */
static const dmr_doc_t DMR_INDEX[DMR_INDEX_N] = {
    {101, {0.90f, 0.10f, 0.00f, 0.00f, 0.10f, 0.05f, 0.00f, 0.00f}},
    {102, {0.10f, 0.90f, 0.05f, 0.00f, 0.00f, 0.05f, 0.10f, 0.00f}},
    {103, {0.05f, 0.05f, 0.90f, 0.10f, 0.05f, 0.00f, 0.00f, 0.10f}},
    {104, {0.00f, 0.05f, 0.10f, 0.90f, 0.05f, 0.05f, 0.00f, 0.00f}},
    {105, {0.05f, 0.00f, 0.00f, 0.10f, 0.90f, 0.10f, 0.05f, 0.00f}},
    {201, {0.50f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f}},
    {202, {0.00f, 0.50f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f}},
    {203, {0.00f, 0.00f, 0.50f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f}},
    {204, {0.00f, 0.00f, 0.00f, 0.50f, 0.50f, 0.00f, 0.00f, 0.00f}},
    {205, {0.00f, 0.00f, 0.00f, 0.00f, 0.50f, 0.50f, 0.00f, 0.00f}},
    {301, {0.30f, 0.30f, 0.30f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f}},
    {302, {0.00f, 0.30f, 0.30f, 0.30f, 0.00f, 0.00f, 0.00f, 0.00f}},
    {303, {0.00f, 0.00f, 0.30f, 0.30f, 0.30f, 0.00f, 0.00f, 0.00f}},
    {304, {0.00f, 0.00f, 0.00f, 0.30f, 0.30f, 0.30f, 0.00f, 0.00f}},
    {305, {0.00f, 0.00f, 0.00f, 0.00f, 0.30f, 0.30f, 0.30f, 0.00f}},
    {401, {0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f}},
    {402, {0.20f, 0.20f, 0.20f, 0.00f, 0.00f, 0.00f, 0.20f, 0.20f}},
    {403, {0.00f, 0.20f, 0.20f, 0.20f, 0.20f, 0.00f, 0.00f, 0.20f}},
    {404, {0.20f, 0.00f, 0.00f, 0.20f, 0.20f, 0.20f, 0.20f, 0.00f}},
    {405, {0.00f, 0.20f, 0.00f, 0.20f, 0.00f, 0.20f, 0.00f, 0.20f}},
};

static const dmr_query_t DMR_QUERIES[DMR_QUERY_N] = {
    {{0.92f, 0.08f, 0.00f, 0.00f, 0.10f, 0.05f, 0.00f, 0.00f}, 101},
    {{0.10f, 0.92f, 0.05f, 0.00f, 0.00f, 0.05f, 0.10f, 0.00f}, 102},
    {{0.05f, 0.05f, 0.91f, 0.10f, 0.05f, 0.00f, 0.00f, 0.10f}, 103},
    {{0.00f, 0.05f, 0.10f, 0.92f, 0.05f, 0.05f, 0.00f, 0.00f}, 104},
    {{0.05f, 0.00f, 0.00f, 0.10f, 0.92f, 0.10f, 0.05f, 0.00f}, 105},
};

typedef struct {
    int unused;
} dmr_ctx_t;

/* ── retrieval ──────────────────────────────────────────────────────────── */

static double cosine_distance(const float *a, const float *b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < DMR_DIM; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    if (na <= 0.0 || nb <= 0.0)
        return 2.0; /* maximally dissimilar */
    double sim = dot / (sqrt(na) * sqrt(nb));
    return 1.0 - sim;
}

/* Returns rank (1-indexed) of `target_id` in cosine-distance ordering. Falls
 * back to DMR_INDEX_N + 1 when target is not present (won't happen here, but
 * keeps the math safe). */
static size_t rank_of_target(const float *query, int64_t target_id) {
    /* Brute-force sort: stable enough at N=20. */
    int64_t ids[DMR_INDEX_N];
    double dists[DMR_INDEX_N];
    for (size_t i = 0; i < DMR_INDEX_N; i++) {
        ids[i] = DMR_INDEX[i].id;
        dists[i] = cosine_distance(query, DMR_INDEX[i].vec);
    }
    /* Insertion sort by ascending distance, break ties by stable order so the
     * rank is deterministic across architectures. */
    for (size_t i = 1; i < DMR_INDEX_N; i++) {
        size_t j = i;
        while (j > 0 && dists[j] < dists[j - 1]) {
            double dt = dists[j];
            dists[j] = dists[j - 1];
            dists[j - 1] = dt;
            int64_t it = ids[j];
            ids[j] = ids[j - 1];
            ids[j - 1] = it;
            j--;
        }
    }
    for (size_t i = 0; i < DMR_INDEX_N; i++) {
        if (ids[i] == target_id)
            return i + 1;
    }
    return DMR_INDEX_N + 1;
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static const char *dmr_name(void *ctx) {
    (void)ctx;
    return "dmr";
}

static bool dmr_available(void *ctx) {
    (void)ctx;
    return true;
}

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

static hu_error_t dmr_run(void *ctx, hu_allocator_t *alloc, hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_evaluation_report_init(alloc, "dmr", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    size_t hit_at_1 = 0, hit_at_5 = 0, hit_at_10 = 0;
    for (size_t i = 0; i < DMR_QUERY_N; i++) {
        size_t r = rank_of_target(DMR_QUERIES[i].query, DMR_QUERIES[i].target_id);
        if (r <= 1)
            hit_at_1++;
        if (r <= 5)
            hit_at_5++;
        if (r <= 10)
            hit_at_10++;
    }

    double r1 = (double)hit_at_1 / (double)DMR_QUERY_N;
    double r5 = (double)hit_at_5 / (double)DMR_QUERY_N;
    double r10 = (double)hit_at_10 / (double)DMR_QUERY_N;

    err = hu_evaluation_report_add_metric(alloc, out, "recall_at_1", r1, DMR_QUERY_N);
    if (err != HU_OK) goto fail;
    err = hu_evaluation_report_add_metric(alloc, out, "recall_at_5", r5, DMR_QUERY_N);
    if (err != HU_OK) goto fail;
    err = hu_evaluation_report_add_metric(alloc, out, "recall_at_10", r10, DMR_QUERY_N);
    if (err != HU_OK) goto fail;

    out->prompts_total = DMR_QUERY_N;
    out->prompts_passed = hit_at_1; /* count "completely correct" by recall@1 */
    out->prompts_failed = DMR_QUERY_N - hit_at_1;
    out->finished_at_ms = now_ms();
    return HU_OK;

fail:
    hu_evaluation_report_free(alloc, out);
    return err;
}

static void dmr_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(dmr_ctx_t));
}

static const hu_evaluation_vtable_t DMR_VTABLE = {
    .name = dmr_name,
    .available = dmr_available,
    .run = dmr_run,
    .deinit = dmr_deinit,
};

hu_error_t hu_evaluation_dmr(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    dmr_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(dmr_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &DMR_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
