/* src/ml/dpo_real_mlx.c — Phase 2 Task 6
 *
 * Apple-only DPO subprocess wrapper around the third-party mlx-lm-lora
 * package (NOT standard mlx-lm — DPO lives at
 * `mlx_lm_lora.trainer.dpo_trainer.train_dpo`). step() exports the
 * preference-pair batch as JSONL with proper RFC 8259 string escaping,
 * then popen()s scripts/dpo_mlx_train.py to spawn the actual MLX run.
 * save_adapter() copies the adapter dir (single-quoted with single-
 * quote rejection to prevent shell injection). deinit() frees the
 * context via the project allocator.
 *
 * In test mode WITHOUT HU_HAVE_MLX_LM (the CI default), step() writes a
 * dummy adapters.safetensors so unit tests can validate path population
 * without downloading Gemma. The full real-MLX adapter validation test
 * (HU_HAVE_MLX_LM=1, ~8GB Gemma download, slow) belongs to Task 7.
 *
 * Plan deviation note: the canonical plan snippet (lines 1632–1813)
 * shows `#include "human/error.h"`. That path does not exist in this
 * repo — the real path is `human/core/error.h` (already pulled in by
 * `human/ml/rl_trainer.h`; including it explicitly here matches the
 * convention used by every other Phase 2 file).
 */
#include "human/ml/rl_trainer.h"
#include "human/ml/dpo_real.h"
#include "human/ml/dpo.h"
#include "human/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char model_id[256];
    char adapter_dir[512];
    double beta;
    size_t max_iters;
} dpo_mlx_ctx_t;

/* Minimal JSON string escaper. RFC 8259 requires escaping " \ and U+0000-U+001F.
 * We escape \" \\ \n \r \t \b \f explicitly and use \uXXXX for the rest of the
 * control range. Output is appended to dst (writes nothing if dst lacks space). */
static size_t json_escape(char *dst, size_t cap, const char *src) {
    size_t w = 0;
    if (cap == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && w + 7 < cap; p++) {
        switch (*p) {
            case '"':  if (w + 2 < cap) { dst[w++]='\\'; dst[w++]='"'; } break;
            case '\\': if (w + 2 < cap) { dst[w++]='\\'; dst[w++]='\\'; } break;
            case '\n': if (w + 2 < cap) { dst[w++]='\\'; dst[w++]='n'; } break;
            case '\r': if (w + 2 < cap) { dst[w++]='\\'; dst[w++]='r'; } break;
            case '\t': if (w + 2 < cap) { dst[w++]='\\'; dst[w++]='t'; } break;
            case '\b': if (w + 2 < cap) { dst[w++]='\\'; dst[w++]='b'; } break;
            case '\f': if (w + 2 < cap) { dst[w++]='\\'; dst[w++]='f'; } break;
            default:
                if (*p < 0x20) {
                    int n = snprintf(dst + w, cap - w, "\\u%04x", *p);
                    if (n < 0 || (size_t)n >= cap - w) return w;
                    w += (size_t)n;
                } else {
                    dst[w++] = (char)*p;
                }
        }
    }
    if (w < cap) dst[w] = '\0';
    return w;
}

/* Write pairs as JSONL to /tmp/hu_dpo_mlx_<pid>.jsonl. Note: hu_preference_pair_t
 * uses fixed-size char arrays (char prompt[2048], etc) per dpo.h:15-26.
 * We MUST JSON-escape every field — synthetic test fixtures are safe but real
 * dpo_pairs rows from chat history routinely contain quotes, newlines, and
 * pasted code blocks that would produce invalid JSONL and cause the Python
 * wrapper's PreferenceDataset() to throw json.JSONDecodeError. */
static hu_error_t write_jsonl(const hu_preference_pair_t *pairs, size_t n,
                              char *out_path, size_t out_path_cap) {
    snprintf(out_path, out_path_cap, "/tmp/hu_dpo_mlx_%d.jsonl", getpid());
    FILE *f = fopen(out_path, "w");
    if (!f) return HU_ERR_IO;
    for (size_t i = 0; i < n; i++) {
        if (pairs[i].prompt_len == 0) continue;
        if (pairs[i].chosen_len == 0 && pairs[i].rejected_len == 0) continue;
        char p_esc[8192], c_esc[16384], r_esc[16384];
        json_escape(p_esc, sizeof(p_esc), pairs[i].prompt);
        json_escape(c_esc, sizeof(c_esc), pairs[i].chosen);
        json_escape(r_esc, sizeof(r_esc), pairs[i].rejected);
        fprintf(f, "{\"prompt\": \"%s\", \"chosen\": \"%s\", \"rejected\": \"%s\"}\n",
                p_esc, c_esc, r_esc);
    }
    fclose(f);
    return HU_OK;
}

static hu_error_t dpo_mlx_step(void *vctx, hu_allocator_t *alloc,
                                const hu_preference_pair_t *pairs, size_t n_pairs,
                                hu_rl_trainer_metrics_t *out) {
    if (!vctx || !alloc || !pairs || n_pairs == 0 || !out) return HU_ERR_INVALID_ARGUMENT;
    dpo_mlx_ctx_t *c = (dpo_mlx_ctx_t *)vctx;

    char jsonl_path[256];
    if (write_jsonl(pairs, n_pairs, jsonl_path, sizeof(jsonl_path)) != HU_OK) return HU_ERR_IO;

    mkdir(c->adapter_dir, 0755);  /* OK if exists */

    /* Match dpo_mlx_save's hardening pattern: reject single-quote in any
     * user-provided field that lands in the popen()'d shell command, then
     * wrap each interpolation in single quotes so spaces / metacharacters
     * in model_id or adapter_dir cannot escape the argument. jsonl_path is
     * derived from getpid() so it is internally safe, but is single-quoted
     * for symmetry. Today's exploitability is low (model_id / adapter_dir
     * are config-owned), but the audit flagged the inconsistency with
     * dpo_mlx_save and we close the gap here. */
    if (strchr(c->model_id, '\'') || strchr(c->adapter_dir, '\'')) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "python3 scripts/dpo_mlx_train.py "
             "--model '%s' "
             "--data '%s' "
             "--adapter-path '%s' "
             "--iters %zu "
             "--beta %.4f "
             "--batch-size 1 "
             "2>&1",
             c->model_id, jsonl_path, c->adapter_dir, c->max_iters, c->beta);

#ifdef HU_IS_TEST
    /* In test mode, write a dummy safetensors file so the test can verify
     * the path is populated. Real subprocess invocation only outside tests
     * unless HU_HAVE_MLX_LM is set explicitly. */
#ifndef HU_HAVE_MLX_LM
    char dummy_path[768];
    snprintf(dummy_path, sizeof(dummy_path), "%s/adapters.safetensors", c->adapter_dir);
    FILE *df = fopen(dummy_path, "wb");
    if (df) { fputs("dummy_safetensors", df); fclose(df); }
    snprintf(out->adapter_path, sizeof(out->adapter_path), "%s", dummy_path);
    out->iters_completed = c->max_iters;
    out->final_loss = 0.0;
    unlink(jsonl_path);
    return HU_OK;
#endif
#endif

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(jsonl_path); return HU_ERR_IO; }
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        /* TODO Phase 5: parse loss/iters from stdout */
    }
    int status = pclose(fp);
    unlink(jsonl_path);
    if (status != 0) return HU_ERR_PROVIDER_RESPONSE;

    snprintf(out->adapter_path, sizeof(out->adapter_path),
             "%s/adapters.safetensors", c->adapter_dir);
    /* Verify file exists and is non-empty */
    struct stat st;
    if (stat(out->adapter_path, &st) != 0 || st.st_size == 0) return HU_ERR_PROVIDER_RESPONSE;

    out->iters_completed = c->max_iters;
    out->final_loss = 0.0;
    return HU_OK;
}

static hu_error_t dpo_mlx_save(void *vctx, hu_allocator_t *alloc, const char *path) {
    /* The mlx-lm-lora train_dpo subprocess writes the adapter directly
     * during step(); save is a copy. Use snprintf-bounded shell-quoted
     * paths to avoid metacharacter issues if adapter_dir contains spaces. */
    if (!vctx || !alloc || !path) return HU_ERR_INVALID_ARGUMENT;
    dpo_mlx_ctx_t *c = (dpo_mlx_ctx_t *)vctx;
    /* Reject paths containing single quote (would escape our quoting). */
    if (strchr(c->adapter_dir, '\'') || strchr(path, '\'')) return HU_ERR_INVALID_ARGUMENT;
    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", c->adapter_dir, path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return HU_ERR_INVALID_ARGUMENT;
    return system(cmd) == 0 ? HU_OK : HU_ERR_IO;
}

static const char *dpo_mlx_name(void *vctx) { (void)vctx; return "dpo_mlx"; }

static void dpo_mlx_deinit(void *vctx, hu_allocator_t *alloc) {
    if (!vctx) return;
    alloc->free(alloc->ctx, vctx, sizeof(dpo_mlx_ctx_t));
}

static const hu_rl_trainer_vtable_t dpo_mlx_vtable = {
    .step = dpo_mlx_step,
    .save_adapter = dpo_mlx_save,
    .name = dpo_mlx_name,
    .deinit = dpo_mlx_deinit,
};

hu_error_t hu_dpo_real_mlx_create(hu_allocator_t *alloc,
                                   const hu_rl_trainer_config_t *config,
                                   hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
#if !defined(__APPLE__)
    return HU_ERR_NOT_SUPPORTED;
#endif
    dpo_mlx_ctx_t *c = (dpo_mlx_ctx_t *)alloc->alloc(alloc->ctx, sizeof(dpo_mlx_ctx_t));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    snprintf(c->model_id, sizeof(c->model_id), "%s",
             config->model_id ? config->model_id : "mlx-community/gemma-3-4b-it-bf16");
    snprintf(c->adapter_dir, sizeof(c->adapter_dir), "%s",
             config->adapter_out_dir ? config->adapter_out_dir : "/tmp/hu_dpo_mlx");
    c->beta = config->beta > 0 ? config->beta : 0.1;
    c->max_iters = config->max_iters > 0 ? config->max_iters : 100;
    out->ctx = c;
    out->vtable = &dpo_mlx_vtable;
    return HU_OK;
}
