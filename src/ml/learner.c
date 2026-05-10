/* W13 — Learning loop: vtable dispatcher + convenience signal builders.
 *
 * Backend selection (`open_default`) prefers the most capable available
 * backend in the order MLX → ggml → CPU. The CPU backend is always
 * available so this call never returns HU_ERR_NOT_SUPPORTED.
 *
 * Signal builders read W3/W4/W5 stores via the underlying graph handle and
 * never write — calling a builder twice with the same store returns the
 * same set of signals (idempotent). Real consumers must track their own
 * watermark by `observed_at` to avoid retraining on stale data. */

#include "human/ml/learner.h"

#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/persona/persona_deltas.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/* Backend vtables come from learner_cpu.c / learner_mlx.c / learner_ggml.c. */
extern const hu_learner_vtable_t hu_learner_cpu_vtable;
extern const hu_learner_vtable_t hu_learner_mlx_vtable;
extern const hu_learner_vtable_t hu_learner_ggml_vtable;

/* Backend factories — each returns HU_OK + a fresh ctx, or
 * HU_ERR_NOT_SUPPORTED if the backend declined to initialise. */
extern hu_error_t hu_learner_cpu_open(hu_allocator_t *alloc, void **out_ctx);
extern hu_error_t hu_learner_mlx_open(hu_allocator_t *alloc, void **out_ctx);
extern hu_error_t hu_learner_ggml_open(hu_allocator_t *alloc, void **out_ctx);

hu_learner_config_t hu_learner_default_config(void) {
    hu_learner_config_t c;
    memset(&c, 0, sizeof(c));
    c.rank = 8;
    c.max_steps = 200;
    c.learning_rate = 1e-4f;
    c.batch_size = 4;
    c.dp_enabled = false;
    c.dp_epsilon = 0.0f;
    c.dp_clip_norm = 0.0f; /* 0 = use default (1.0) */
    c.budget_ms = 60000;   /* 60s default soft budget */
    c.seed = 0;            /* backend supplies a deterministic default */
    snprintf(c.model_version, sizeof(c.model_version), "v1");
    return c;
}

/* ── W15 DP privacy accountant ────────────────────────────────────────── */

void hu_dp_accountant_init(hu_dp_accountant_t *a, double delta) {
    if (!a)
        return;
    a->epsilon_spent = 0.0;
    a->delta = delta > 0.0 ? delta : 1e-5;
    a->queries_count = 0;
}

void hu_dp_accountant_record_query(hu_dp_accountant_t *a, double epsilon_step) {
    if (!a || epsilon_step <= 0.0)
        return;
    a->epsilon_spent += epsilon_step;
    a->queries_count++;
}

double hu_dp_accountant_total_epsilon(const hu_dp_accountant_t *a) {
    if (!a)
        return 0.0;
    return a->epsilon_spent;
}

static hu_error_t open_with(hu_allocator_t *alloc, const hu_learner_vtable_t *vt,
                            hu_error_t (*opener)(hu_allocator_t *, void **),
                            hu_learner_t **out) {
    void *ctx = NULL;
    hu_error_t e = opener(alloc, &ctx);
    if (e != HU_OK)
        return e;
    hu_learner_t *l = (hu_learner_t *)alloc->alloc(alloc->ctx, sizeof(*l));
    if (!l) {
        if (vt->deinit)
            vt->deinit(ctx);
        return HU_ERR_OUT_OF_MEMORY;
    }
    /* Zero-init: ensures the bridge's pending fields start NULL/zero so the
     * first emit allocates lazily and the close path frees cleanly. */
    memset(l, 0, sizeof(*l));
    l->vt = vt;
    l->ctx = ctx;
    l->alloc = alloc;
    *out = l;
    return HU_OK;
}

hu_error_t hu_learner_open_default(hu_allocator_t *alloc, hu_learner_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (hu_learner_mlx_vtable.available && hu_learner_mlx_vtable.available()) {
        if (open_with(alloc, &hu_learner_mlx_vtable, hu_learner_mlx_open, out) == HU_OK)
            return HU_OK;
    }
    if (hu_learner_ggml_vtable.available && hu_learner_ggml_vtable.available()) {
        if (open_with(alloc, &hu_learner_ggml_vtable, hu_learner_ggml_open, out) == HU_OK)
            return HU_OK;
    }
    /* CPU fallback is the contract — must always succeed. */
    return open_with(alloc, &hu_learner_cpu_vtable, hu_learner_cpu_open, out);
}

hu_error_t hu_learner_open_named(hu_allocator_t *alloc, const char *backend_name,
                                 hu_learner_t **out) {
    if (!alloc || !backend_name || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (strcmp(backend_name, "cpu") == 0)
        return open_with(alloc, &hu_learner_cpu_vtable, hu_learner_cpu_open, out);
    if (strcmp(backend_name, "mlx") == 0) {
        if (!hu_learner_mlx_vtable.available || !hu_learner_mlx_vtable.available())
            return HU_ERR_NOT_SUPPORTED;
        return open_with(alloc, &hu_learner_mlx_vtable, hu_learner_mlx_open, out);
    }
    if (strcmp(backend_name, "ggml") == 0) {
        if (!hu_learner_ggml_vtable.available || !hu_learner_ggml_vtable.available())
            return HU_ERR_NOT_SUPPORTED;
        return open_with(alloc, &hu_learner_ggml_vtable, hu_learner_ggml_open, out);
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_learner_train(hu_learner_t *l, const hu_learner_config_t *cfg,
                            const hu_training_signal_t *signals, size_t signals_count,
                            hu_learner_report_t *out_report) {
    if (!l || !l->vt || !l->vt->train || !cfg || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    if (signals_count > 0 && !signals)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = l->vt->train(l->ctx, cfg, signals, signals_count, out_report);
    if (e == HU_OK && cfg->dp_enabled && cfg->dp_epsilon > 0.0f) {
        if (l->dp_accountant.queries_count == 0)
            hu_dp_accountant_init(&l->dp_accountant, 1e-5);
        hu_dp_accountant_record_query(&l->dp_accountant, (double)cfg->dp_epsilon);
    }
    return e;
}

void hu_learner_close(hu_learner_t *l) {
    if (!l)
        return;
    if (l->vt && l->vt->deinit)
        l->vt->deinit(l->ctx);
    if (l->alloc) {
        if (l->pending) {
            l->alloc->free(l->alloc->ctx, l->pending,
                           l->pending_cap * sizeof(*l->pending));
            l->pending = NULL;
        }
        l->alloc->free(l->alloc->ctx, l, sizeof(*l));
    }
}

void hu_learner_signals_free(hu_allocator_t *alloc, hu_training_signal_t *signals,
                             size_t count) {
    if (!alloc || !signals)
        return;
    alloc->free(alloc->ctx, signals, count * sizeof(*signals));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Signal builders.
 * ──────────────────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

/* Allocate a signals array of `count` (may be zero). */
static hu_error_t alloc_signals(hu_allocator_t *alloc, size_t count,
                                hu_training_signal_t **out) {
    if (count == 0) {
        *out = NULL;
        return HU_OK;
    }
    hu_training_signal_t *s =
        (hu_training_signal_t *)alloc->alloc(alloc->ctx, count * sizeof(*s));
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    memset(s, 0, count * sizeof(*s));
    *out = s;
    return HU_OK;
}

static hu_error_t signals_from_deltas_with_status(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_persona_delta_status_t status,
                                                  bool as_dpo_dispreferred,
                                                  hu_training_signal_t **out,
                                                  size_t *out_count) {
    if (!m || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_ERR_INVALID_ARGUMENT;

    hu_persona_delta_t *deltas = NULL;
    size_t n = 0;
    hu_error_t e = hu_persona_delta_list(g, alloc, contact_id, cid_len, status, 64, &deltas, &n);
    if (e != HU_OK)
        return e;

    hu_training_signal_t *signals = NULL;
    e = alloc_signals(alloc, n, &signals);
    if (e != HU_OK) {
        hu_persona_delta_free(alloc, deltas, n);
        return e;
    }

    for (size_t i = 0; i < n; i++) {
        signals[i].observed_at = deltas[i].proposed_at_ms;
        if (as_dpo_dispreferred) {
            signals[i].kind = HU_TRAIN_DPO_PAIR;
            /* Synthesise a placeholder prompt that lets the DPO loss
             * meaningfully separate the absent (preferred) and present
             * (dispreferred) renderings. */
            snprintf(signals[i].as.dpo.prompt, sizeof(signals[i].as.dpo.prompt),
                     "[verifier-flag kind=%d key=%s]",
                     (int)deltas[i].kind, deltas[i].key);
            /* preferred: explicit "abstain" — the response the verifier
             * would have allowed. dispreferred: the rejected delta value. */
            snprintf(signals[i].as.dpo.preferred, sizeof(signals[i].as.dpo.preferred),
                     "[abstain]");
            snprintf(signals[i].as.dpo.dispreferred, sizeof(signals[i].as.dpo.dispreferred),
                     "%s", deltas[i].value);
            float w = deltas[i].confidence;
            if (w <= 0.0f)
                w = 0.5f;
            signals[i].as.dpo.weight = w;
        } else {
            signals[i].kind = HU_TRAIN_PERSONA_DELTA;
            signals[i].as.persona.delta = deltas[i];
        }
    }

    hu_persona_delta_free(alloc, deltas, n);
    *out = signals;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_learner_signals_from_verifier_flags(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_training_signal_t **out,
                                                  size_t *out_count) {
    return signals_from_deltas_with_status(m, alloc, contact_id, cid_len,
                                           HU_DELTA_STATUS_QUARANTINED,
                                           /*as_dpo_dispreferred=*/true, out, out_count);
}

hu_error_t hu_learner_signals_from_persona_deltas(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_training_signal_t **out,
                                                  size_t *out_count) {
    return signals_from_deltas_with_status(m, alloc, contact_id, cid_len,
                                           HU_DELTA_STATUS_APPLIED,
                                           /*as_dpo_dispreferred=*/false, out, out_count);
}

/* Map a free-form outcome string to a reward in [0, 1]. The tokens we look
 * for mirror the strings persisted by hu_case_record() callers: "ok",
 * "good", "success" (positive); "bad", "failed", "pushed back" (negative).
 * Anything else lands at neutral 0.5. Case-insensitive substring match. */
static float reward_from_outcome(const char *outcome) {
    if (!outcome || !*outcome)
        return 0.5f;
    char buf[128];
    size_t i = 0;
    for (; i + 1 < sizeof(buf) && outcome[i]; i++)
        buf[i] = (char)tolower((unsigned char)outcome[i]);
    buf[i] = '\0';
    if (strstr(buf, "ok") || strstr(buf, "good") || strstr(buf, "success") ||
        strstr(buf, "great") || strstr(buf, "happy"))
        return 1.0f;
    if (strstr(buf, "bad") || strstr(buf, "failed") || strstr(buf, "fail") ||
        strstr(buf, "pushed back") || strstr(buf, "rejected") || strstr(buf, "wrong"))
        return 0.0f;
    return 0.5f;
}

hu_error_t hu_learner_signals_from_case_outcomes(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                                 const char *contact_id, size_t cid_len,
                                                 hu_training_signal_t **out,
                                                 size_t *out_count) {
    if (!m || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    /* The case_records schema is owned by src/agent/case_based.c. Issue a
     * benign CREATE TABLE IF NOT EXISTS so we tolerate a graph that has
     * never seen hu_case_record(). */
    static const char *kDdl =
        "CREATE TABLE IF NOT EXISTS case_records ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "goal_verb TEXT NOT NULL,"
        "anchor_entity_ids TEXT NOT NULL DEFAULT '',"
        "plan_text TEXT,"
        "outcome TEXT,"
        "happened_at INTEGER NOT NULL)";
    sqlite3_stmt *st_ddl = NULL;
    if (sqlite3_prepare_v2(db, kDdl, -1, &st_ddl, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    if (sqlite3_step(st_ddl) != SQLITE_DONE) {
        sqlite3_finalize(st_ddl);
        return HU_ERR_IO;
    }
    sqlite3_finalize(st_ddl);

    /* Two-pass: count, then fetch. Keeps the array exact-sized. */
    sqlite3_stmt *st = NULL;
    const char *kSelect =
        "SELECT id, outcome, happened_at FROM case_records "
        "WHERE contact_id = ? ORDER BY happened_at DESC LIMIT 256";
    if (sqlite3_prepare_v2(db, kSelect, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id, (int)cid_len, SQLITE_STATIC);

    /* Cap at 256 cases per call; matches the LIMIT above. */
    hu_training_signal_t scratch[256];
    memset(scratch, 0, sizeof(scratch));
    size_t n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < 256) {
        int64_t case_id = sqlite3_column_int64(st, 0);
        const char *outcome = (const char *)sqlite3_column_text(st, 1);
        int64_t happened = sqlite3_column_int64(st, 2);
        scratch[n].kind = HU_TRAIN_CASE_OUTCOME;
        scratch[n].as.case_outcome.case_id = case_id;
        scratch[n].as.case_outcome.reward = reward_from_outcome(outcome);
        scratch[n].observed_at = happened;
        n++;
    }
    sqlite3_finalize(st);

    hu_training_signal_t *signals = NULL;
    hu_error_t e = alloc_signals(alloc, n, &signals);
    if (e != HU_OK)
        return e;
    if (n > 0)
        memcpy(signals, scratch, n * sizeof(scratch[0]));
    *out = signals;
    *out_count = n;
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_learner_signals_from_verifier_flags(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_training_signal_t **out,
                                                  size_t *out_count) {
    (void)m;
    (void)alloc;
    (void)contact_id;
    (void)cid_len;
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_learner_signals_from_persona_deltas(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_training_signal_t **out,
                                                  size_t *out_count) {
    (void)m;
    (void)alloc;
    (void)contact_id;
    (void)cid_len;
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_learner_signals_from_case_outcomes(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                                 const char *contact_id, size_t cid_len,
                                                 hu_training_signal_t **out,
                                                 size_t *out_count) {
    (void)m;
    (void)alloc;
    (void)contact_id;
    (void)cid_len;
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
