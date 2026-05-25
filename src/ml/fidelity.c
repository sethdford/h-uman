#include "human/ml/fidelity.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Track D D2.2 — persona-fidelity computation primitives.
 *
 * Both `human ml fidelity-status` and the gateway's
 * `metrics.fidelity` method delegate the math here so the
 * dashboard tile, the CLI output, and any future telemetry
 * agree on a single definition of "baseline mean". The two
 * surfaces only differ in *where* the resulting JSON gets
 * delivered and *how* the persona name is resolved. */

hu_error_t hu_ml_fidelity_resolve_target(hu_allocator_t *alloc,
                                         hu_communication_style_t *out_target,
                                         bool *out_synthetic) {
    (void)alloc;
    if (!out_target || !out_synthetic)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_target, 0, sizeof(*out_target));
    *out_synthetic = true;

    /* Try to load the user's accumulated communication-style
     * fingerprint from `~/.human/personal_model.bin`. A zero-sample
     * style is treated as "no real data yet" and falls through. */
    char pm_path[1024];
    if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
        hu_personal_model_t loaded;
        if (hu_personal_model_load(&loaded, pm_path) == HU_OK && loaded.style.sample_count > 0U) {
            *out_target = loaded.style;
            *out_synthetic = false;
            return HU_OK;
        }
    }

    /* Synthetic fallback — same defaults as `human ml lora-baseline`
     * so the two surfaces stay numerically comparable. Any change here
     * MUST be mirrored in `lora-baseline`'s synthetic path or the
     * `check-lora-baseline.sh` gate will diverge from `metrics.fidelity`. */
    out_target->formality = 0.3f;
    out_target->verbosity = 0.5f;
    out_target->emoji_frequency = 0.2f;
    out_target->humor_receptivity = 0.6f;
    out_target->lowercase_ratio = 0.85f;
    out_target->abbreviation_ratio = 0.2f;
    out_target->avg_message_length = 60;
    out_target->sample_count = 1;
    return HU_OK;
}

hu_error_t hu_ml_fidelity_score_baseline(const hu_persona_t *persona,
                                         const hu_communication_style_t *target,
                                         hu_communication_style_set_summary_t *out_summary) {
    if (!persona || !target || !out_summary)
        return HU_ERR_INVALID_ARGUMENT;
    if (target->sample_count == 0U)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->min_score = 1.f;

    size_t scored = 0;
    size_t skipped = 0;
    float sum = 0.f;
    float mn = 1.f;
    float mx = 0.f;
    for (size_t b = 0; b < persona->example_banks_count; b++) {
        const hu_persona_example_bank_t *bank = &persona->example_banks[b];
        for (size_t i = 0; i < bank->examples_count; i++) {
            const char *r = bank->examples[i].response;
            if (!r || !r[0]) {
                skipped++;
                continue;
            }
            float s = hu_communication_style_fidelity_score(target, r, strlen(r));
            if (s < 0.f) {
                skipped++;
                continue;
            }
            sum += s;
            if (s < mn)
                mn = s;
            if (s > mx)
                mx = s;
            scored++;
        }
    }
    out_summary->scored = scored;
    out_summary->skipped = skipped;
    if (scored == 0) {
        out_summary->mean = 0.f;
        out_summary->min_score = 0.f;
        out_summary->max_score = 0.f;
    } else {
        out_summary->mean = sum / (float)scored;
        out_summary->min_score = mn;
        out_summary->max_score = mx;
    }
    return HU_OK;
}

double hu_communication_style_fidelity_score_delta(double baseline_score, double adapted_score,
                                                   const hu_communication_style_t *target) {
    (void)target; /* reserved for future per-target weighting */

    /* Validate input range: both scores should be in [0, 1] */
    if (baseline_score < 0.0 || baseline_score > 1.0 || adapted_score < 0.0 ||
        adapted_score > 1.0) {
        hu_log_warn("fidelity", NULL,
                    "fidelity_score_delta: invalid input range (baseline=%.3f, adapted=%.3f); "
                    "returning 0.0",
                    baseline_score, adapted_score);
        return 0.0;
    }

    /* Return simple difference: positive = improvement toward target */
    return adapted_score - baseline_score;
}

/* ── US-7.6 — judgment-fidelity (INS-A) ─────────────────────────────────
 *
 * Process-wide NLL-compute seam. Tests inject a deterministic mock via
 * `hu_ml_fidelity_set_nll_compute_fn`; production callers (none yet —
 * sprint 7 ships this dormant per decision D3) leave the default in
 * place, which returns HU_ERR_NOT_SUPPORTED so the gate plumbing
 * declares itself inactive instead of silently passing. */

static hu_error_t default_nll_not_supported(const char *prompt, size_t prompt_len,
                                            const char *continuation, size_t continuation_len,
                                            void *ctx, double *out_nll) {
    (void)prompt;
    (void)prompt_len;
    (void)continuation;
    (void)continuation_len;
    (void)ctx;
    if (out_nll)
        *out_nll = 0.0;
    return HU_ERR_NOT_SUPPORTED;
}

static hu_ml_nll_compute_fn_t g_nll_fn = default_nll_not_supported;
static void *g_nll_ctx = NULL;

void hu_ml_fidelity_set_nll_compute_fn(hu_ml_nll_compute_fn_t fn, void *ctx) {
    g_nll_fn = fn ? fn : default_nll_not_supported;
    g_nll_ctx = fn ? ctx : NULL;
}

const char *hu_ml_fidelity_default_holdout_path(void) {
    const char *env = getenv("HU_JUDGMENT_HOLDOUT");
    if (env && env[0])
        return env;
    return "tests/fixtures/judgment_fidelity_holdout.jsonl";
}

/* Append a row by copying both strings via the allocator. On any
 * allocation failure the partially-constructed row is rolled back. */
static hu_error_t holdout_append_row(hu_allocator_t *alloc, hu_ml_judgment_holdout_t *out,
                                     const char *prompt, size_t prompt_len,
                                     const char *continuation, size_t continuation_len) {
    if (out->rows_count == out->rows_capacity) {
        size_t new_cap = out->rows_capacity ? out->rows_capacity * 2 : 8;
        void *new_rows = alloc->realloc(alloc->ctx, out->rows,
                                        out->rows_capacity * sizeof(hu_ml_judgment_holdout_row_t),
                                        new_cap * sizeof(hu_ml_judgment_holdout_row_t));
        if (!new_rows)
            return HU_ERR_OUT_OF_MEMORY;
        out->rows = (hu_ml_judgment_holdout_row_t *)new_rows;
        out->rows_capacity = new_cap;
    }
    char *p = (char *)alloc->alloc(alloc->ctx, prompt_len + 1);
    if (!p)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(p, prompt, prompt_len);
    p[prompt_len] = '\0';
    char *c = (char *)alloc->alloc(alloc->ctx, continuation_len + 1);
    if (!c) {
        alloc->free(alloc->ctx, p, prompt_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(c, continuation, continuation_len);
    c[continuation_len] = '\0';
    hu_ml_judgment_holdout_row_t *row = &out->rows[out->rows_count++];
    row->prompt = p;
    row->prompt_len = prompt_len;
    row->continuation = c;
    row->continuation_len = continuation_len;
    return HU_OK;
}

hu_error_t hu_ml_fidelity_load_holdout(hu_allocator_t *alloc, const char *path,
                                       hu_ml_judgment_holdout_t *out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_IO;

    /* Read entire file. JSONL holdout fixtures are small (≤ a few KB). */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    rewind(fp);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t read_n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read_n] = '\0';

    /* Iterate line by line. Malformed / non-object / missing-field
     * lines are silently dropped so the loader is robust to in-line
     * comments and stray whitespace; the test fixture documents which
     * rows are intentionally malformed. */
    size_t i = 0;
    hu_error_t err = HU_OK;
    while (i < read_n) {
        size_t line_start = i;
        while (i < read_n && buf[i] != '\n')
            i++;
        size_t line_len = i - line_start;
        if (i < read_n)
            i++; /* consume newline */

        /* trim leading whitespace */
        size_t s = line_start;
        size_t e = line_start + line_len;
        while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r'))
            s++;
        while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\r'))
            e--;
        if (s == e)
            continue; /* blank line */
        if (buf[s] == '#' || buf[s] == '/')
            continue; /* comment line (# … or // …) */

        hu_json_value_t *parsed = NULL;
        if (hu_json_parse(alloc, buf + s, e - s, &parsed) != HU_OK || !parsed)
            continue;
        if (parsed->type != HU_JSON_OBJECT) {
            hu_json_free(alloc, parsed);
            continue;
        }
        const char *prompt = hu_json_get_string(parsed, "prompt");
        const char *cont = hu_json_get_string(parsed, "continuation");
        if (!prompt || !cont) {
            hu_json_free(alloc, parsed);
            continue;
        }
        err = holdout_append_row(alloc, out, prompt, strlen(prompt), cont, strlen(cont));
        hu_json_free(alloc, parsed);
        if (err != HU_OK)
            break;
    }

    alloc->free(alloc->ctx, buf, (size_t)size + 1);
    if (err != HU_OK) {
        hu_ml_fidelity_free_holdout(alloc, out);
        return err;
    }
    return HU_OK;
}

void hu_ml_fidelity_free_holdout(hu_allocator_t *alloc, hu_ml_judgment_holdout_t *holdout) {
    if (!alloc || !holdout)
        return;
    for (size_t i = 0; i < holdout->rows_count; i++) {
        hu_ml_judgment_holdout_row_t *r = &holdout->rows[i];
        if (r->prompt)
            alloc->free(alloc->ctx, r->prompt, r->prompt_len + 1);
        if (r->continuation)
            alloc->free(alloc->ctx, r->continuation, r->continuation_len + 1);
    }
    if (holdout->rows)
        alloc->free(alloc->ctx, holdout->rows,
                    holdout->rows_capacity * sizeof(hu_ml_judgment_holdout_row_t));
    memset(holdout, 0, sizeof(*holdout));
}

hu_error_t hu_ml_fidelity_score_judgment(const hu_ml_judgment_holdout_t *holdout,
                                         hu_ml_judgment_summary_t *out_summary) {
    if (!holdout || !out_summary)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_summary, 0, sizeof(*out_summary));

    size_t scored = 0;
    size_t skipped = 0;
    double sum = 0.0;
    double mn = 0.0;
    double mx = 0.0;
    bool seen = false;

    for (size_t i = 0; i < holdout->rows_count; i++) {
        const hu_ml_judgment_holdout_row_t *r = &holdout->rows[i];
        if (!r->prompt || !r->continuation) {
            skipped++;
            continue;
        }
        double nll = 0.0;
        hu_error_t err = g_nll_fn(r->prompt, r->prompt_len, r->continuation, r->continuation_len,
                                  g_nll_ctx, &nll);
        if (err == HU_ERR_NOT_SUPPORTED) {
            skipped++;
            continue;
        }
        if (err != HU_OK)
            return err;
        sum += nll;
        if (!seen) {
            mn = nll;
            mx = nll;
            seen = true;
        } else {
            if (nll < mn)
                mn = nll;
            if (nll > mx)
                mx = nll;
        }
        scored++;
    }

    out_summary->scored = scored;
    out_summary->skipped = skipped;
    if (scored == 0) {
        out_summary->mean_nll = 0.0;
        out_summary->min_nll = 0.0;
        out_summary->max_nll = 0.0;
        out_summary->available = false;
    } else {
        out_summary->mean_nll = sum / (double)scored;
        out_summary->min_nll = mn;
        out_summary->max_nll = mx;
        out_summary->available = true;
    }
    return HU_OK;
}
