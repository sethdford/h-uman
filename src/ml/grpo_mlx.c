/* src/ml/grpo_mlx.c — Phase 4 Task 8 (RL SOTA)
 *
 * Apple-only GRPO subprocess wrapper around the third-party mlx-lm-lora
 * package. Mirrors src/ml/kto_mlx.c (Phase 3 HARDENED pattern — round-3
 * critic H2) at the structural level, NOT the Phase 2 dpo_real_mlx.c
 * legacy:
 *
 *   - O_EXCL + 0600 JSONL writes (no symlink overwrite, owner-RW only)
 *   - Canonical specific-symbol probe (mlx_lm_lora.trainer.grpo_trainer
 *     .train_grpo) — partial installs surface at create-time, not popen
 *     time deep in step()
 *   - HU_IS_TEST short-circuit on the create-time probe (round-3
 *     critic M7) — test-mode never spawns the python probe subprocess
 *   - Single-quote rejection guard on user-controlled paths (shell-
 *     injection defense for the popen call)
 *   - HU_E2E_TEST_MODE=1 env propagation so the python wrapper writes
 *     a 0-byte sentinel adapter_model.safetensors instead of running
 *     the real (Gemma-downloading) training. Triggered under HU_IS_TEST
 *     when HU_HAVE_MLX_LM_GRPO is NOT defined; suppressed under
 *     HU_HAVE_MLX_LM_GRPO=1 so the gated test_grpo_mlx_subprocess_
 *     produces_safetensors run actually trains.
 *
 * GRPO ingestion: ONLY the prompt is written to the JSONL. chosen/
 * rejected are deliberately skipped — GRPO samples its rollouts from
 * the live policy inside mlx-lm-lora; pre-supplied completions are
 * meaningless to it (umbrella §3 ship contract).
 *
 * Conditional-compilation contract (round-3 critic L3 — strict C11):
 * defining HU_GRPO_HAVE_MLX_IMPL=1 here makes Task 0's #ifndef-guarded
 * stub in src/ml/rl_trainer.c fall out of the link, leaving exactly one
 * definition of hu_grpo_mlx_create per binary.
 */
#define HU_GRPO_HAVE_MLX_IMPL 1

#include "human/ml/grpo.h"
#include "human/ml/rl_trainer.h"
#include "human/core/error.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char model_id[256];
    char adapter_dir[512];
    size_t n_rollouts;
    double clip_eps;
    double kl_beta;
    size_t max_iters;
} grpo_mlx_ctx_t;

/* --- Public test seams (declared in src/ml/grpo_mlx_priv.h) ----------
 *
 * Two test-only entrypoints live below.  They exist because Task 8's
 * design constraints are otherwise mutually exclusive:
 *   - the create-time probe must short-circuit under HU_IS_TEST
 *     (round-3 critic M7 — no python3 probe subprocesses from tests)
 *   - the dummy-adapter test must exercise grpo_mlx_step end-to-end
 *
 * Without a test seam, the M7 short-circuit would make every code path
 * that reaches popen() unreachable under HU_IS_TEST.  Forward
 * declarations are provided here (not in the public grpo.h header) so
 * the test file picks them up via the existing
 * target_include_directories(human_tests PRIVATE ${HU_ROOT}/src/ml)
 * (see CMakeLists.txt:3082 — the same mechanism Phase 3 RM and Phase 4
 * GRPO-loss tests use). */
hu_error_t hu_grpo_mlx_write_jsonl_for_test(const char *out_path,
                                             const hu_preference_pair_t *pairs,
                                             size_t n_pairs);
#if HU_IS_TEST
hu_error_t hu_grpo_mlx_create_for_test(hu_allocator_t *alloc,
                                        const hu_rl_trainer_config_t *config,
                                        hu_rl_trainer_t *out);
#endif

/* Canonical specific-symbol probe — matches the
 * mlx_lm_lora_grpo_available() helper in src/ml/rl_trainer.c (D10 +
 * R1). The probe text is intentionally identical to the dispatcher's
 * so a partial install (top-level module imports, grpo_trainer
 * submodule missing) surfaces here at create-time, not at popen() time
 * mid-step.
 *
 * Under HU_IS_TEST we short-circuit to "unavailable" (round-3 critic
 * M7) so the deterministic / no-side-effects contract in AGENTS.md §3
 * isn't violated by an unconditional python3 probe subprocess. Tests
 * that need to exercise step() opt in via the
 * hu_grpo_mlx_create_for_test seam below. */
static int mlx_lm_lora_grpo_available(void) {
#if HU_IS_TEST
    return 0;
#else
    if (system("python3 -c 'from mlx_lm_lora.trainer.grpo_trainer "
               "import train_grpo' 2>/dev/null") == 0) {
        return 1;
    }
    /* CLI-module fallback — matches the rl_trainer.c dispatcher probe. */
    return system("python3 -m mlx_lm_lora.train --help 2>/dev/null "
                  "| grep -q 'train-mode\\|grpo'") == 0;
#endif
}

/* Minimal JSON string escaper — same logic as src/ml/kto_mlx.c::
 * kto_json_escape and src/ml/dpo_real_mlx.c. Kept as a private copy
 * per Rule of Three (3 users now, each with subtly different field
 * layouts; collapsing to a shared cross-module helper without a
 * value-add proposal is over-abstraction). */
static size_t grpo_json_escape(char *dst, size_t cap, const char *src) {
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

/* Write GRPO prompts as JSONL. Schema: {"prompt": "..."} — chosen/
 * rejected DELIBERATELY skipped (GRPO samples rollouts from the live
 * policy; pre-supplied completions are meaningless).
 *
 * Hardened path (mirrors src/ml/kto_mlx.c::kto_write_jsonl — round-3
 * critic H2): O_EXCL + 0600 mode defeats symlink-attack on the
 * user-controlled out_path slot AND limits exposure to other local
 * users on shared CI runners / workstations. Predictable PID-derived
 * path is acceptable because O_EXCL fails on pre-placed symlinks and
 * grpo_mlx_step unlinks the file after popen() completes.
 *
 * Public for the JSONL-secure-perms test in tests/test_grpo_mlx.c
 * (test_grpo_mlx_jsonl_write_uses_secure_perms validates the 0600
 * mode contract unconditionally — even without HU_HAVE_MLX_LM_GRPO,
 * the hardening must be in effect). */
hu_error_t hu_grpo_mlx_write_jsonl_for_test(const char *out_path,
                                             const hu_preference_pair_t *pairs,
                                             size_t n_pairs) {
    if (!out_path || (!pairs && n_pairs > 0)) return HU_ERR_INVALID_ARGUMENT;
    int fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        /* Stale file from a prior PID-collision run — unlink and retry
         * once. Matches kto_write_jsonl's single-retry contract. */
        unlink(out_path);
        fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) return HU_ERR_IO;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        /* F4 (end-gate audit): the O_EXCL open already created the
         * 0600 file at out_path.  A bare close(fd) here would leak the
         * empty file in /tmp.  Unlink before returning so subsequent
         * runs on the same PID (predictable jsonl_path) don't trip the
         * O_EXCL retry-once contract. */
        close(fd);
        unlink(out_path);
        return HU_ERR_IO;
    }
    for (size_t i = 0; i < n_pairs; i++) {
        if (pairs[i].prompt_len == 0) continue;
        char p_esc[8192];
        grpo_json_escape(p_esc, sizeof(p_esc), pairs[i].prompt);
        /* ONLY prompt — chosen/rejected ignored. */
        fprintf(f, "{\"prompt\": \"%s\"}\n", p_esc);
    }
    fclose(f);
    return HU_OK;
}

/* Internal alias for the production call site — folds the indirection
 * at compile time. */
static hu_error_t grpo_write_jsonl(const char *out_path,
                                    const hu_preference_pair_t *pairs,
                                    size_t n_pairs) {
    return hu_grpo_mlx_write_jsonl_for_test(out_path, pairs, n_pairs);
}

static hu_error_t grpo_mlx_step(void *vctx, hu_allocator_t *alloc,
                                 const hu_preference_pair_t *pairs, size_t n_pairs,
                                 hu_rl_trainer_metrics_t *out) {
    if (!vctx || !alloc || !pairs || n_pairs == 0 || !out) return HU_ERR_INVALID_ARGUMENT;
    grpo_mlx_ctx_t *c = (grpo_mlx_ctx_t *)vctx;

    char jsonl_path[256];
    snprintf(jsonl_path, sizeof(jsonl_path), "/tmp/hu_grpo_mlx_%d.jsonl", getpid());
    hu_error_t werr = grpo_write_jsonl(jsonl_path, pairs, n_pairs);
    if (werr != HU_OK) return werr;

    /* F5 (end-gate audit): the unchecked mkdir return previously
     * masked filesystem errors (EACCES, ENOSPC, ENAMETOOLONG, …) as a
     * later opaque stat() failure post-popen.  EEXIST is the expected
     * benign case — the test fixtures reuse adapter_dir across runs. */
    if (mkdir(c->adapter_dir, 0700) == -1 && errno != EEXIST) {
        unlink(jsonl_path);
        return HU_ERR_IO;
    }

    /* Single-quote rejection — defends the popen shell-string against
     * injection via user-controlled paths/ids. Same guard as Phase 2
     * dpo_real_mlx.c and Phase 3 kto_mlx.c; intentionally conservative
     * (legitimate paths don't contain single quotes). */
    if (strchr(c->model_id, '\'') || strchr(c->adapter_dir, '\'')) {
        unlink(jsonl_path);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char cmd[2048];
    /* The script forwards --n-rollouts, --clip-eps, --kl-beta, --iters
     * to mlx-lm-lora's GRPO trainer (grpo-group-size / grpo-epsilon /
     * grpo-beta CLI flags). --backbone-path is the model id; --input
     * is the JSONL just written. */
    snprintf(cmd, sizeof(cmd),
             "python3 scripts/grpo_mlx_train.py "
             "--input '%s' "
             "--adapter-out '%s' "
             "--backbone-path '%s' "
             "--n-rollouts %zu "
             "--clip-eps %.6f "
             "--kl-beta %.6f "
             "--iters %zu "
             "--reward-fn synthetic "
             "2>&1",
             jsonl_path, c->adapter_dir, c->model_id,
             c->n_rollouts, c->clip_eps, c->kl_beta, c->max_iters);

    /* Push HU_E2E_TEST_MODE=1 into the popen environment under HU_IS_TEST
     * (but NOT under HU_HAVE_MLX_LM_GRPO — that test runs real training).
     * The Python wrapper sees the env var and writes a 0-byte sentinel
     * adapter_model.safetensors, skipping the real Gemma-downloading
     * subprocess. Mirrors Phase 3 KTO's dummy-adapter path but moved into
     * Python so the C side stays backend-agnostic.
     *
     * #if HU_IS_TEST (round-3 critic M3 + M4): the repo standard is the
     * numeric check, not #ifdef. The build system uses HU_IS_TEST=0 in
     * non-test targets to disable test-mode; #ifdef would always activate. */
#if HU_IS_TEST
#if !defined(HU_HAVE_MLX_LM_GRPO) || HU_HAVE_MLX_LM_GRPO == 0
    setenv("HU_E2E_TEST_MODE", "1", /*overwrite=*/1);
#endif
#endif

    FILE *fp = popen(cmd, "r");

#if HU_IS_TEST
#if !defined(HU_HAVE_MLX_LM_GRPO) || HU_HAVE_MLX_LM_GRPO == 0
    /* Don't leak HU_E2E_TEST_MODE to subsequent popen calls from other
     * trainers in the same test process. */
    unsetenv("HU_E2E_TEST_MODE");
#endif
#endif

    if (!fp) { unlink(jsonl_path); return HU_ERR_IO; }

    char buf[1024];
    double last_loss = 0.0;
    size_t last_iter = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        double parsed_loss = 0.0;
        unsigned long parsed_iter = 0;
        /* Same stdout-parse contract as src/ml/dpo_real_mlx.c +
         * src/ml/kto_mlx.c — accept the three common log shapes
         * mlx-lm-lora emits across versions. */
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

    /* Populate output metrics. adapter_path is the canonical mlx-lm-lora
     * artifact name (adapter_model.safetensors — note the "_model"
     * infix; this matches the Python wrapper's output contract and
     * differs from Phase 3 KTO which writes "adapters.safetensors"). */
    snprintf(out->adapter_path, sizeof(out->adapter_path),
             "%s/adapter_model.safetensors", c->adapter_dir);
    struct stat st;
    if (stat(out->adapter_path, &st) != 0) return HU_ERR_PROVIDER_RESPONSE;
    /* Under HU_IS_TEST + HU_E2E_TEST_MODE shortcut we accept the 0-byte
     * sentinel; the real-MLX path (HU_HAVE_MLX_LM_GRPO=1) validates
     * non-zero st_size at the test level (see
     * test_grpo_mlx_subprocess_produces_safetensors). */
    out->iters_completed = last_iter > 0 ? last_iter : c->max_iters;
    out->final_loss = last_loss;
    return HU_OK;
}

static hu_error_t grpo_mlx_save(void *vctx, hu_allocator_t *alloc, const char *path) {
    if (!vctx || !alloc || !path) return HU_ERR_INVALID_ARGUMENT;
    grpo_mlx_ctx_t *c = (grpo_mlx_ctx_t *)vctx;
    if (strchr(c->adapter_dir, '\'') || strchr(path, '\''))
        return HU_ERR_INVALID_ARGUMENT;
    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", c->adapter_dir, path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return HU_ERR_INVALID_ARGUMENT;
    return system(cmd) == 0 ? HU_OK : HU_ERR_IO;
}

static const char *grpo_mlx_name(void *vctx) { (void)vctx; return "grpo_mlx"; }

static void grpo_mlx_deinit(void *vctx, hu_allocator_t *alloc) {
    if (!vctx) return;
    alloc->free(alloc->ctx, vctx, sizeof(grpo_mlx_ctx_t));
}

static const hu_rl_trainer_vtable_t grpo_mlx_vtable = {
    .step = grpo_mlx_step,
    .save_adapter = grpo_mlx_save,
    .name = grpo_mlx_name,
    .deinit = grpo_mlx_deinit,
};

/* Shared constructor body — both the production factory and the
 * test-only seam below populate the ctx identically; only the probe
 * gate differs. */
static hu_error_t grpo_mlx_construct_ctx(hu_allocator_t *alloc,
                                          const hu_rl_trainer_config_t *config,
                                          hu_rl_trainer_t *out) {
    grpo_mlx_ctx_t *c = (grpo_mlx_ctx_t *)alloc->alloc(alloc->ctx, sizeof(grpo_mlx_ctx_t));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    snprintf(c->model_id, sizeof(c->model_id), "%s",
             config->model_id ? config->model_id : "mlx-community/gemma-3-4b-it-bf16");
    snprintf(c->adapter_dir, sizeof(c->adapter_dir), "%s",
             config->adapter_out_dir ? config->adapter_out_dir : "/tmp/hu_grpo_mlx");
    /* Same default-sentinel convention as the HUML factory: n_rollouts=0
     * → 4 (umbrella §5 ship contract), clip_eps=0 → 0.2 (trl default),
     * kl_beta<0 → 0.04 (DeepSeek R1; the MED-1 R3 sentinel for "use
     * literature default"). kl_beta == 0 is preserved verbatim and
     * disables the KL penalty downstream. */
    c->n_rollouts = config->n_rollouts > 0 ? config->n_rollouts : 4;
    c->clip_eps = config->clip_eps > 0.0 ? config->clip_eps : 0.2;
    c->kl_beta = config->kl_beta < 0.0 ? 0.04 : config->kl_beta;
    c->max_iters = config->max_iters > 0 ? config->max_iters : 50;
    out->ctx = c;
    out->vtable = &grpo_mlx_vtable;
    return HU_OK;
}

hu_error_t hu_grpo_mlx_create(hu_allocator_t *alloc,
                               const hu_rl_trainer_config_t *config,
                               hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
#if !defined(__APPLE__)
    return HU_ERR_NOT_SUPPORTED;
#endif
    /* Under HU_IS_TEST the probe short-circuits to "unavailable" (M7).
     * test_grpo_mlx_factory_unavailable_when_python_probe_fails pins
     * this contract. Tests that need step() exercise it via the
     * for-test seam below. */
    if (!mlx_lm_lora_grpo_available()) return HU_ERR_NOT_SUPPORTED;
    return grpo_mlx_construct_ctx(alloc, config, out);
}

#if HU_IS_TEST
/* Test-only factory seam — bypasses the M7 probe short-circuit so
 * test_grpo_mlx_dummy_adapter_in_test_mode can exercise step() under
 * HU_IS_TEST. The Python wrapper's HU_E2E_TEST_MODE=1 shortcut (which
 * step() sets via setenv when !HU_HAVE_MLX_LM_GRPO) writes a 0-byte
 * sentinel instead of running real training; the test asserts the
 * sentinel exists. Never compiled into production binaries. */
hu_error_t hu_grpo_mlx_create_for_test(hu_allocator_t *alloc,
                                        const hu_rl_trainer_config_t *config,
                                        hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
#if !defined(__APPLE__)
    return HU_ERR_NOT_SUPPORTED;
#endif
    return grpo_mlx_construct_ctx(alloc, config, out);
}
#endif
