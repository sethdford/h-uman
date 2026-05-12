/* src/ml/kto_mlx.c — Phase 3 Task 7
 *
 * Apple-only KTO subprocess wrapper around the third-party mlx-lm-lora
 * package. Mirrors src/ml/dpo_real_mlx.c structurally but with KTO's
 * one-sided signal schema: each pair is either desirable (chosen_len > 0)
 * or undesirable (rejected_len > 0), serialized as
 * {"prompt": "...", "completion": "...", "label": true/false}.
 *
 * In test mode WITHOUT HU_HAVE_MLX_LM_KTO, step() writes a dummy
 * adapters.safetensors so unit tests can validate path population
 * without downloading a model.
 */
#include "human/ml/kto.h"
#include "human/ml/rl_trainer.h"
#include "human/core/error.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char model_id[256];
    char adapter_dir[512];
    double beta;
    double lambda_d;
    double lambda_u;
    size_t max_iters;
} kto_mlx_ctx_t;

/* Phase 3 audit fold-in (auditor F2 + Task 0 D6): probe the specific KTO
 * trainer symbol, not just the generic `mlx_lm_lora.train` module. A
 * partial install (module imports, trainer symbol missing) was Phase 2's
 * deferred-failure root cause; matching the DPO probe pattern in
 * src/ml/dpo_real_mlx.c::mlx_lm_lora_available eliminates it for KTO too. */
static int mlx_lm_lora_kto_available(void) {
    return system(
        "python3 -c 'from mlx_lm_lora.trainer.kto_trainer "
        "import train_kto, KTOTrainingArgs' 2>/dev/null") == 0;
}

/* Minimal JSON string escaper — same logic as dpo_real_mlx.c.
 * Kept as a private copy per Rule of Three (only 2 users so far). */
static size_t kto_json_escape(char *dst, size_t cap, const char *src) {
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

/* Write KTO signals as JSONL to /tmp/hu_kto_mlx_<pid>.jsonl.
 * Schema: {"prompt": "...", "completion": "...", "label": true/false}
 *
 * Phase 3 audit fold-in (critic HIGH-1): two-sided pairs (chosen AND
 * rejected populated) are silently skipped to match the HUML backend's
 * behavior at src/ml/kto.c::kto_huml_step. Without this guard the MLX
 * path would write the chosen side as label=true and silently drop the
 * rejected side — same input batch produces different training signal
 * across backends. KTO is one-sided by definition; mixed batches are
 * a caller-side error.
 *
 * Phase 3 audit fold-in (critic MEDIUM-1): O_EXCL + 0600 mode prevents
 * symlink-attack and limits exposure to other local users on shared
 * macOS CI runners or workstations. Predictable PID is acceptable here
 * because O_EXCL fails on pre-placed symlink + the file is removed
 * after popen() completes (kto_mlx_step does unlink(jsonl_path)). */
static hu_error_t kto_write_jsonl(const hu_preference_pair_t *pairs, size_t n,
                                   char *out_path, size_t out_path_cap) {
    snprintf(out_path, out_path_cap, "/tmp/hu_kto_mlx_%d.jsonl", getpid());
    int fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        /* Stale file from previous PID-collision run — unlink and retry once. */
        unlink(out_path);
        fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) return HU_ERR_IO;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return HU_ERR_IO; }
    for (size_t i = 0; i < n; i++) {
        if (pairs[i].prompt_len == 0) continue;
        int is_desirable   = (pairs[i].chosen_len > 0);
        int is_undesirable = (pairs[i].rejected_len > 0);
        int is_two_sided   = (is_desirable && is_undesirable);
        if (is_two_sided) continue;
        if (!is_desirable && !is_undesirable) continue;

        const char *completion = is_desirable ? pairs[i].chosen : pairs[i].rejected;
        const char *label_str = is_desirable ? "true" : "false";

        char p_esc[8192], c_esc[16384];
        kto_json_escape(p_esc, sizeof(p_esc), pairs[i].prompt);
        kto_json_escape(c_esc, sizeof(c_esc), completion);
        fprintf(f, "{\"prompt\": \"%s\", \"completion\": \"%s\", \"label\": %s}\n",
                p_esc, c_esc, label_str);
    }
    fclose(f);
    return HU_OK;
}

static hu_error_t kto_mlx_step(void *vctx, hu_allocator_t *alloc,
                                const hu_preference_pair_t *pairs, size_t n_pairs,
                                hu_rl_trainer_metrics_t *out) {
    if (!vctx || !alloc || !pairs || n_pairs == 0 || !out) return HU_ERR_INVALID_ARGUMENT;
    kto_mlx_ctx_t *c = (kto_mlx_ctx_t *)vctx;

    char jsonl_path[256];
    if (kto_write_jsonl(pairs, n_pairs, jsonl_path, sizeof(jsonl_path)) != HU_OK)
        return HU_ERR_IO;

    mkdir(c->adapter_dir, 0755);

    if (strchr(c->model_id, '\'') || strchr(c->adapter_dir, '\'')) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "python3 scripts/kto_mlx_train.py "
             "--model '%s' "
             "--data '%s' "
             "--adapter-path '%s' "
             "--iters %zu "
             "--beta %.4f "
             "--lambda-d %.4f "
             "--lambda-u %.4f "
             "2>&1",
             c->model_id, jsonl_path, c->adapter_dir,
             c->max_iters, c->beta, c->lambda_d, c->lambda_u);

/* Phase 3 audit fold-in (critic MEDIUM-3): use `#if HU_IS_TEST` (numeric
 * check) — the repo standard. `#ifdef HU_IS_TEST` would activate this
 * dummy-write path even when the build system explicitly defines
 * HU_IS_TEST=0 to disable test mode. See reward_model_train.c:26 for
 * the same standardization (Phase 2 audit b6a71f81). */
#if HU_IS_TEST
#ifndef HU_HAVE_MLX_LM_KTO
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
    double last_loss = 0.0;
    size_t last_iter = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        double parsed_loss = 0.0;
        unsigned long parsed_iter = 0;
        if (sscanf(buf, "Iter %lu: Val loss %lf", &parsed_iter, &parsed_loss) == 2 ||
            sscanf(buf, "Iter %lu, loss: %lf", &parsed_iter, &parsed_loss) == 2 ||
            sscanf(buf, "iter %lu: loss=%lf", &parsed_iter, &parsed_loss) == 2) {
            last_loss = parsed_loss;
            last_iter = (size_t)parsed_iter;
        }
    }
    int status = pclose(fp);
    unlink(jsonl_path);
    if (status != 0) return HU_ERR_PROVIDER_RESPONSE;

    snprintf(out->adapter_path, sizeof(out->adapter_path),
             "%s/adapters.safetensors", c->adapter_dir);
    struct stat st;
    if (stat(out->adapter_path, &st) != 0 || st.st_size == 0)
        return HU_ERR_PROVIDER_RESPONSE;

    out->iters_completed = last_iter > 0 ? last_iter : c->max_iters;
    out->final_loss = last_loss;
    return HU_OK;
}

static hu_error_t kto_mlx_save(void *vctx, hu_allocator_t *alloc, const char *path) {
    if (!vctx || !alloc || !path) return HU_ERR_INVALID_ARGUMENT;
    kto_mlx_ctx_t *c = (kto_mlx_ctx_t *)vctx;
    if (strchr(c->adapter_dir, '\'') || strchr(path, '\''))
        return HU_ERR_INVALID_ARGUMENT;
    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", c->adapter_dir, path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return HU_ERR_INVALID_ARGUMENT;
    return system(cmd) == 0 ? HU_OK : HU_ERR_IO;
}

static const char *kto_mlx_name(void *vctx) { (void)vctx; return "kto_mlx"; }

static void kto_mlx_deinit(void *vctx, hu_allocator_t *alloc) {
    if (!vctx) return;
    alloc->free(alloc->ctx, vctx, sizeof(kto_mlx_ctx_t));
}

static const hu_rl_trainer_vtable_t kto_mlx_vtable = {
    .step = kto_mlx_step,
    .save_adapter = kto_mlx_save,
    .name = kto_mlx_name,
    .deinit = kto_mlx_deinit,
};

hu_error_t hu_kto_mlx_create(hu_allocator_t *alloc,
                              const hu_rl_trainer_config_t *config,
                              hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
#if !defined(__APPLE__)
    return HU_ERR_NOT_SUPPORTED;
#endif
    if (!mlx_lm_lora_kto_available()) return HU_ERR_NOT_SUPPORTED;
    kto_mlx_ctx_t *c = (kto_mlx_ctx_t *)alloc->alloc(alloc->ctx, sizeof(kto_mlx_ctx_t));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    snprintf(c->model_id, sizeof(c->model_id), "%s",
             config->model_id ? config->model_id : "mlx-community/gemma-3-4b-it-bf16");
    snprintf(c->adapter_dir, sizeof(c->adapter_dir), "%s",
             config->adapter_out_dir ? config->adapter_out_dir : "/tmp/hu_kto_mlx");
    c->beta = config->beta > 0 ? config->beta : 0.1;
    c->lambda_d = config->lambda_d > 0 ? config->lambda_d : 1.0;
    c->lambda_u = config->lambda_u > 0 ? config->lambda_u : 1.0;
    c->max_iters = config->max_iters > 0 ? config->max_iters : 100;
    out->ctx = c;
    out->vtable = &kto_mlx_vtable;
    return HU_OK;
}
