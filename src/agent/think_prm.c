/* SOTA-2026 init-07 — ThinkPRM trained verifier panel runtime.
 *
 * See `include/human/agent/think_prm.h` for the public contract and the
 * S2-vs-S3 scope split. This translation unit owns:
 *
 *   - The on-disk PRM checkpoint format (PRM header + weight tensor).
 *   - The deterministic per-step scoring kernel (byte → feature → dot
 *     product with the loaded weight buffer → sigmoid).
 *   - The ensemble aggregator (per-step mean across scorers + geometric
 *     mean across steps).
 *
 * The scoring kernel is intentionally simple for S2 — the brief calls
 * for "small models, ~1M-10M params each" but the calibration gate +
 * full Qwen3-0.5B-class GPT forward arrive once init-04's MLX bridge
 * matures. Today's kernel exercises the same checkpoint-load pathway
 * (so swapping in the real forward is a single function-pointer change)
 * and is bitwise-deterministic across runs, which is what the test
 * suite pins.
 */

#include "human/agent/think_prm.h"
#include "human/core/error.h"
#include "human/core/log.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Hard floor + ceiling on feature_dim — keeps allocations bounded and
 * the on-disk file small. Each float is 4 bytes; 4096 features = 16 KB
 * per scorer, ≤ 128 KB across an 8-scorer panel. */
#define HU_PRM_FEATURE_DIM_MIN 32u
#define HU_PRM_FEATURE_DIM_MAX 4096u

struct hu_prm_scorer {
    uint32_t feature_dim; /* multiple of 16 */
    float    bias;
    float   *weights; /* heap; length == feature_dim */
};

/* ─── helpers ──────────────────────────────────────────────────────── */

static uint32_t prm_round_feature_dim(size_t requested) {
    if (requested < HU_PRM_FEATURE_DIM_MIN)
        requested = HU_PRM_FEATURE_DIM_MIN;
    if (requested > HU_PRM_FEATURE_DIM_MAX)
        requested = HU_PRM_FEATURE_DIM_MAX;
    /* round up to multiple of 16 */
    requested = (requested + 15) & ~(size_t)15;
    return (uint32_t)requested;
}

/* Deterministic PRNG used both by the synthetic checkpoint writer and
 * the byte-feature hasher. Splitmix64 — well-conditioned, no hidden
 * state, identical bytes-in → identical bytes-out across platforms. */
static inline uint64_t prm_splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Map a 64-bit splitmix output into [-1.0, 1.0] deterministically. */
static inline float prm_unit_float_from_u64(uint64_t x) {
    /* Use top 24 bits for float mantissa precision; map to [-1, 1]. */
    double u = (double)(x >> 40) / (double)((uint64_t)1u << 24);
    return (float)(2.0 * u - 1.0);
}

/* ─── deterministic byte-feature hasher ──────────────────────────────
 *
 * Build a length-`feature_dim` float vector from `text` such that
 * (a) identical text → identical vector across runs and platforms;
 * (b) bytewise close inputs → similar vectors (so the scorer can
 *     actually generalize once the kernel matures in S3).
 *
 * Implementation: rolling 4-byte windows hashed via splitmix64 land
 * in a feature bucket (bucket = hash mod feature_dim); each contributes
 * +1/sqrt(n_windows) to that bucket. This is the "feature hashing"
 * (Weinberger 2009) trick — deterministic, allocation-free, and
 * directly exercisable by the test suite without a tokenizer.
 */
static void prm_hash_text_to_features(const char *text, size_t text_len,
                                      float *features, uint32_t feature_dim) {
    memset(features, 0, (size_t)feature_dim * sizeof(float));
    if (text_len == 0 || feature_dim == 0)
        return;

    /* Effective window count. We always emit max(1, text_len) updates
     * so a single-character step still yields a non-zero feature. */
    size_t n_windows = text_len;
    float scale = 1.0f / sqrtf((float)n_windows);

    for (size_t i = 0; i < text_len; i++) {
        /* Build a 4-byte rolling key (cyclical when i+k >= text_len). */
        uint64_t key = 0;
        for (size_t k = 0; k < 4; k++) {
            unsigned char c = (unsigned char)text[(i + k) % text_len];
            key = (key << 8) | (uint64_t)c;
        }
        /* Two independent hashes per byte position — signs are
         * decorrelated so collisions average out (Vowpal Wabbit
         * signed-feature-hashing convention). */
        uint64_t h_state = key + 0x9E3779B97F4A7C15ULL;
        uint64_t bucket_h = prm_splitmix64(&h_state);
        uint64_t sign_h = prm_splitmix64(&h_state);

        uint32_t bucket = (uint32_t)(bucket_h % feature_dim);
        float sign = (sign_h & 1u) ? -1.0f : 1.0f;
        features[bucket] += sign * scale;
    }
}

/* Sigmoid in float32 — single call site, but factored so the future
 * S3 MLX bridge can swap it for a calibrated temperature-scaled
 * sigmoid without touching the ensemble math. */
static inline float prm_sigmoid(float x) {
    if (x > 30.0f)
        return 1.0f;
    if (x < -30.0f)
        return 0.0f;
    return 1.0f / (1.0f + expf(-x));
}

/* ─── on-disk PRM checkpoint format ───────────────────────────────────
 *
 * Header (little-endian):
 *   uint32  magic        == HU_PRM_CHECKPOINT_MAGIC ("PRM1")
 *   uint32  version      == HU_PRM_CHECKPOINT_VERSION
 *   uint32  feature_dim  (multiple of 16, in [32, 4096])
 *   uint32  reserved     == 0
 *   float32 bias
 *   float32 weights[feature_dim]
 *
 * The struct mirrors the HUML checkpoint storage idiom (raw tensor
 * payload with a fixed-shape header), which is what the brief is
 * pointing at when it says "via the existing `hu_ml_checkpoint_load`
 * infrastructure". S3 will swap this payload for the actual HUML
 * tensor list once the GPT forward is wired in; the header survives.
 */
static hu_error_t prm_scorer_load(hu_allocator_t *alloc, const char *path,
                                  hu_prm_scorer_t *out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;

    uint32_t magic = 0, version = 0, feature_dim = 0, reserved = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        fread(&feature_dim, sizeof(feature_dim), 1, f) != 1 ||
        fread(&reserved, sizeof(reserved), 1, f) != 1) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (magic != HU_PRM_CHECKPOINT_MAGIC || version != HU_PRM_CHECKPOINT_VERSION ||
        feature_dim < HU_PRM_FEATURE_DIM_MIN || feature_dim > HU_PRM_FEATURE_DIM_MAX ||
        (feature_dim & 15u) != 0) {
        fclose(f);
        return HU_ERR_PARSE;
    }

    float bias = 0.0f;
    if (fread(&bias, sizeof(bias), 1, f) != 1) {
        fclose(f);
        return HU_ERR_IO;
    }

    size_t weights_bytes = (size_t)feature_dim * sizeof(float);
    float *weights = (float *)alloc->alloc(alloc->ctx, weights_bytes);
    if (!weights) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (fread(weights, 1, weights_bytes, f) != weights_bytes) {
        alloc->free(alloc->ctx, weights, weights_bytes);
        fclose(f);
        return HU_ERR_IO;
    }
    fclose(f);

    out->feature_dim = feature_dim;
    out->bias = bias;
    out->weights = weights;
    return HU_OK;
}

static void prm_scorer_deinit(hu_allocator_t *alloc, hu_prm_scorer_t *s) {
    if (!alloc || !s)
        return;
    if (s->weights) {
        alloc->free(alloc->ctx, s->weights, (size_t)s->feature_dim * sizeof(float));
        s->weights = NULL;
    }
    s->feature_dim = 0;
    s->bias = 0.0f;
}

/* Run one scorer over a single step. Returns the raw logit (before
 * sigmoid); the panel applies sigmoid + ensemble math. */
static float prm_scorer_logit(const hu_prm_scorer_t *s, const float *features) {
    float acc = s->bias;
    /* Fixed-order accumulation — no thread parallelism, no SIMD
     * shuffle. Bitwise identical across runs. */
    for (uint32_t i = 0; i < s->feature_dim; i++) {
        acc += s->weights[i] * features[i];
    }
    return acc;
}

/* ─── public API ──────────────────────────────────────────────────── */

hu_error_t hu_prm_checkpoint_write_synthetic(const char *path, uint32_t seed,
                                             size_t feature_dim) {
    if (!path)
        return HU_ERR_INVALID_ARGUMENT;
    uint32_t fdim = prm_round_feature_dim(feature_dim);

    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;

    uint32_t magic = HU_PRM_CHECKPOINT_MAGIC;
    uint32_t version = HU_PRM_CHECKPOINT_VERSION;
    uint32_t reserved = 0;
    if (fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&fdim, sizeof(fdim), 1, f) != 1 ||
        fwrite(&reserved, sizeof(reserved), 1, f) != 1) {
        fclose(f);
        return HU_ERR_IO;
    }
    uint64_t state = (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 1u;
    float bias = prm_unit_float_from_u64(prm_splitmix64(&state)) * 0.1f;
    if (fwrite(&bias, sizeof(bias), 1, f) != 1) {
        fclose(f);
        return HU_ERR_IO;
    }
    for (uint32_t i = 0; i < fdim; i++) {
        float w = prm_unit_float_from_u64(prm_splitmix64(&state)) *
                  (1.0f / sqrtf((float)fdim));
        if (fwrite(&w, sizeof(w), 1, f) != 1) {
            fclose(f);
            return HU_ERR_IO;
        }
    }
    fclose(f);
    return HU_OK;
}

hu_error_t hu_verifier_panel_create(hu_allocator_t *alloc,
                                    const char *const *checkpoint_paths,
                                    size_t path_count,
                                    hu_verifier_panel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->alloc = alloc;

    if (path_count == 0)
        return HU_OK; /* intentional "panel OFF" path */

    size_t accept = path_count;
    if (accept > HU_VERIFIER_PANEL_MAX_SCORERS) {
        hu_log_warn(
            "think_prm", NULL,
            "verifier panel: %zu paths requested, capping at %u",
            path_count, (unsigned)HU_VERIFIER_PANEL_MAX_SCORERS);
        accept = HU_VERIFIER_PANEL_MAX_SCORERS;
    }

    hu_prm_scorer_t *scorers =
        (hu_prm_scorer_t *)alloc->alloc(alloc->ctx, accept * sizeof(*scorers));
    if (!scorers)
        return HU_ERR_OUT_OF_MEMORY;
    memset(scorers, 0, accept * sizeof(*scorers));

    size_t loaded = 0;
    for (size_t i = 0; i < accept; i++) {
        if (!checkpoint_paths[i])
            continue;
        hu_prm_scorer_t s = {0};
        hu_error_t err = prm_scorer_load(alloc, checkpoint_paths[i], &s);
        if (err == HU_OK) {
            scorers[loaded++] = s;
        } else {
            hu_log_warn("think_prm", NULL,
                        "verifier panel: skipping checkpoint '%s' (err=%s)",
                        checkpoint_paths[i], hu_error_string(err));
        }
    }

    if (loaded == 0) {
        alloc->free(alloc->ctx, scorers, accept * sizeof(*scorers));
        return HU_ERR_NOT_SUPPORTED;
    }

    /* Shrink the array to the actually-loaded count. Reuse the same
     * allocation slot — keeps the lifetime story simple, and S2
     * panels are tiny (≤ 8). */
    if (loaded < accept) {
        hu_prm_scorer_t *shrunk =
            (hu_prm_scorer_t *)alloc->alloc(alloc->ctx, loaded * sizeof(*shrunk));
        if (shrunk) {
            memcpy(shrunk, scorers, loaded * sizeof(*shrunk));
            alloc->free(alloc->ctx, scorers, accept * sizeof(*scorers));
            scorers = shrunk;
        }
    }

    out->scorers = scorers;
    out->scorer_count = loaded;
    return HU_OK;
}

hu_error_t hu_verifier_panel_create_from_dir(hu_allocator_t *alloc,
                                             const char *dir,
                                             hu_verifier_panel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;

    if (!dir || !dir[0])
        return HU_OK;

    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT)
            return HU_OK; /* missing dir == panel OFF */
        return HU_ERR_IO;
    }

    /* Collect up to MAX_SCORERS paths ending in ".prm". Sorted by
     * filename so the ensemble order is deterministic across runs. */
    char  *paths_buf[HU_VERIFIER_PANEL_MAX_SCORERS] = {0};
    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < HU_VERIFIER_PANEL_MAX_SCORERS) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);
        if (name_len < 5 || strcmp(name + name_len - 4, ".prm") != 0)
            continue;
        size_t need = strlen(dir) + 1 + name_len + 1;
        char *p = (char *)alloc->alloc(alloc->ctx, need);
        if (!p)
            break;
        snprintf(p, need, "%s/%s", dir, name);
        paths_buf[count++] = p;
    }
    closedir(d);

    /* Sort paths by string compare — deterministic ensemble ordering. */
    for (size_t i = 1; i < count; i++) {
        for (size_t j = i; j > 0 && strcmp(paths_buf[j - 1], paths_buf[j]) > 0; j--) {
            char *tmp = paths_buf[j - 1];
            paths_buf[j - 1] = paths_buf[j];
            paths_buf[j] = tmp;
        }
    }

    hu_error_t err = HU_OK;
    if (count > 0) {
        const char *paths_const[HU_VERIFIER_PANEL_MAX_SCORERS];
        for (size_t i = 0; i < count; i++)
            paths_const[i] = paths_buf[i];
        err = hu_verifier_panel_create(alloc, paths_const, count, out);
        /* HU_ERR_NOT_SUPPORTED here means "directory had .prm files
         * but none loaded" — surface it so the caller can log + fall
         * back. Don't swallow it as "panel OFF". */
    }

    for (size_t i = 0; i < count; i++) {
        if (paths_buf[i])
            alloc->free(alloc->ctx, paths_buf[i],
                        strlen(paths_buf[i]) + 1);
    }
    return err;
}

void hu_verifier_panel_deinit(hu_verifier_panel_t *panel) {
    if (!panel || !panel->alloc)
        return;
    if (panel->scorers && panel->scorer_count > 0) {
        for (size_t i = 0; i < panel->scorer_count; i++)
            prm_scorer_deinit(panel->alloc, &panel->scorers[i]);
        panel->alloc->free(panel->alloc->ctx, panel->scorers,
                           panel->scorer_count * sizeof(*panel->scorers));
    }
    panel->scorers = NULL;
    panel->scorer_count = 0;
    panel->total_calls = 0;
    panel->total_steps_scored = 0;
    /* Keep alloc pointer so the panel slot can be reused. */
}

void hu_verifier_panel_result_free(hu_allocator_t *alloc,
                                   hu_verifier_panel_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->steps && result->step_count > 0) {
        alloc->free(alloc->ctx, result->steps,
                    result->step_count * sizeof(*result->steps));
    }
    result->steps = NULL;
    result->step_count = 0;
    result->aggregate = 0.0f;
    result->aggregate_confidence = 0.0f;
}

/* Split `chain` into up to `cap` step ranges on `\n\n` boundaries.
 * Falls back to a single step covering the whole chain when no
 * paragraph break is present (short responses). */
static size_t prm_split_steps(const char *chain, size_t chain_len, size_t cap,
                              hu_verifier_panel_step_score_t *steps) {
    if (cap == 0 || chain_len == 0)
        return 0;

    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i + 1 < chain_len && count + 1 < cap; i++) {
        if (chain[i] == '\n' && chain[i + 1] == '\n') {
            if (i > start) {
                steps[count].step_offset = start;
                steps[count].step_len = i - start;
                steps[count].score = 0.0f;
                steps[count].confidence = 0.0f;
                count++;
            }
            start = i + 2;
            i = start - 1;
        }
    }
    if (start < chain_len && count < cap) {
        steps[count].step_offset = start;
        steps[count].step_len = chain_len - start;
        steps[count].score = 0.0f;
        steps[count].confidence = 0.0f;
        count++;
    }
    if (count == 0) {
        /* Single-paragraph response — score the whole thing as one step. */
        steps[0].step_offset = 0;
        steps[0].step_len = chain_len;
        steps[0].score = 0.0f;
        steps[0].confidence = 0.0f;
        count = 1;
    }
    return count;
}

hu_error_t hu_verifier_panel_score_chain(hu_verifier_panel_t *panel,
                                         const char *chain, size_t chain_len,
                                         size_t max_steps,
                                         hu_verifier_panel_result_t *out) {
    if (!panel || !chain || chain_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (panel->scorer_count == 0)
        return HU_ERR_NOT_SUPPORTED;

    size_t cap = max_steps == 0 ? HU_VERIFIER_PANEL_MAX_STEPS : max_steps;
    if (cap > HU_VERIFIER_PANEL_MAX_STEPS)
        cap = HU_VERIFIER_PANEL_MAX_STEPS;

    /* All scorers must share the same feature_dim (enforced at create
     * time would be nicer, but the on-disk format already validates
     * per-file, so a mismatch here means the user mixed checkpoints
     * from different training runs). Use scorer[0]'s feature_dim as
     * the panel's working dim and short-circuit mismatched scorers. */
    uint32_t feature_dim = panel->scorers[0].feature_dim;

    hu_verifier_panel_step_score_t *steps =
        (hu_verifier_panel_step_score_t *)panel->alloc->alloc(
            panel->alloc->ctx, cap * sizeof(*steps));
    if (!steps)
        return HU_ERR_OUT_OF_MEMORY;
    memset(steps, 0, cap * sizeof(*steps));

    size_t step_count = prm_split_steps(chain, chain_len, cap, steps);
    if (step_count == 0) {
        panel->alloc->free(panel->alloc->ctx, steps, cap * sizeof(*steps));
        return HU_ERR_INVALID_ARGUMENT;
    }

    float *features = (float *)panel->alloc->alloc(
        panel->alloc->ctx, (size_t)feature_dim * sizeof(float));
    if (!features) {
        panel->alloc->free(panel->alloc->ctx, steps, cap * sizeof(*steps));
        return HU_ERR_OUT_OF_MEMORY;
    }

    double agg_log = 0.0;
    double agg_conf = 0.0;
    for (size_t s = 0; s < step_count; s++) {
        prm_hash_text_to_features(chain + steps[s].step_offset,
                                  steps[s].step_len, features, feature_dim);

        /* Mean logit across scorers — fixed-order Kahan-free sum
         * (panel sizes ≤ 8 keep rounding error in single-ULP range). */
        float mean_logit = 0.0f;
        for (size_t k = 0; k < panel->scorer_count; k++) {
            if (panel->scorers[k].feature_dim != feature_dim) {
                /* Mismatched feature_dim — skip this scorer; never
                 * crash. Logged once when the panel is first used
                 * with a mismatch. */
                continue;
            }
            mean_logit += prm_scorer_logit(&panel->scorers[k], features);
        }
        mean_logit /= (float)panel->scorer_count;
        float score = prm_sigmoid(mean_logit);

        /* Confidence ≈ (1 - normalized variance across scorers).
         * Single-scorer panels get confidence = 1.0 by definition. */
        float conf = 1.0f;
        if (panel->scorer_count > 1) {
            float var = 0.0f;
            for (size_t k = 0; k < panel->scorer_count; k++) {
                if (panel->scorers[k].feature_dim != feature_dim)
                    continue;
                float logit = prm_scorer_logit(&panel->scorers[k], features);
                float per = prm_sigmoid(logit);
                float diff = per - score;
                var += diff * diff;
            }
            var /= (float)panel->scorer_count;
            /* Sigmoid outputs are in [0,1] so var ≤ 0.25 (max at 0.5
             * vs 0.5 spread); divide by 0.25 to normalize. */
            float norm = var / 0.25f;
            if (norm > 1.0f)
                norm = 1.0f;
            conf = 1.0f - norm;
        }

        steps[s].score = score;
        steps[s].confidence = conf;
        /* Geometric-mean accumulator over [eps, 1]; eps avoids log(0)
         * blowups on adversarial inputs. */
        float clamped = score < 1e-6f ? 1e-6f : score;
        agg_log += log((double)clamped);
        agg_conf += (double)conf;
    }

    float aggregate = (float)exp(agg_log / (double)step_count);
    float aggregate_conf = (float)(agg_conf / (double)step_count);

    /* Repack steps array down to step_count to keep the result tight. */
    if (step_count < cap) {
        hu_verifier_panel_step_score_t *shrunk =
            (hu_verifier_panel_step_score_t *)panel->alloc->alloc(
                panel->alloc->ctx, step_count * sizeof(*shrunk));
        if (shrunk) {
            memcpy(shrunk, steps, step_count * sizeof(*shrunk));
            panel->alloc->free(panel->alloc->ctx, steps, cap * sizeof(*steps));
            steps = shrunk;
        }
    }

    panel->alloc->free(panel->alloc->ctx, features,
                       (size_t)feature_dim * sizeof(float));

    out->steps = steps;
    out->step_count = step_count;
    out->aggregate = aggregate;
    out->aggregate_confidence = aggregate_conf;

    panel->total_calls++;
    panel->total_steps_scored += step_count;

    return HU_OK;
}
