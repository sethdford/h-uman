/* src/ml/lora_subprocess.c
 *
 * mlx_lm.lora subprocess driver. See header for contract.
 * Sprint B residuals N1 (2026-05-25). */

#include "human/ml/lora_subprocess.h"

#include "human/core/log.h"
#include "human/core/process_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── default hyperparameters ────────────────────────────────────────── */
/*
 * Defaults match the M3 runbook (docs/guides/m3-bridge-runbook.md):
 *   batch_size=4   conservative for 16-32GB Apple Silicon
 *   iters=200      ~5-15 min on M-series for a few hundred pairs
 *   lora_layers=8  conservative starting point for tiny datasets
 *
 * THESE ARE STARTING-POINT TUNINGS. They're explicitly modest because
 * the first user runs will have <500 pairs and overfit risk dominates.
 * Once a deployment has >1000 pairs, the runbook recommends bumping
 * iters and layers. Defaults here only fire when the caller passes
 * 0 — explicit values from the caller always win.
 */
#define HU_LORA_DEFAULT_BATCH_SIZE  4
#define HU_LORA_DEFAULT_ITERS       200
#define HU_LORA_DEFAULT_LORA_LAYERS 8

/* ────────────────────────────────────────────────────────────────────
 *
 * POLICY DECISIONS (chosen 2026-05-25):
 *
 *   TIMEOUT = 30 min (1800s)
 *     Covers normal M-series training of a few hundred pairs
 *     (~5-15 min) with ~3x headroom. Long enough that real runs
 *     finish; short enough that a hung process doesn't block
 *     tomorrow's nightly slot.
 *
 *   RETRIES = 1 (2 attempts max)
 *     Catches transient failures (model download flake, momentary
 *     OOM) without amplifying systemic ones. Worst-case wall-clock:
 *     2×timeout + 30s backoff = ~61 min.
 *
 *   BACKOFF = fixed 30s
 *     Simple, predictable. Exponential adds wall-clock variance to
 *     the nightly slot without measurably better recovery — LoRA
 *     failure modes (OOM, bad data, wrong hparams) don't fix
 *     themselves between attempts.
 *
 * These are DEFAULTS only — every value is per-call overridable via
 * hu_lora_subprocess_config_t. The daemon's nightly tick uses these
 * defaults; the CLI runbook lets the user tune them ad-hoc.
 * ──────────────────────────────────────────────────────────────────── */
#define HU_LORA_DEFAULT_TIMEOUT_SEC     1800u
#define HU_LORA_DEFAULT_MAX_RETRIES     1
#define HU_LORA_DEFAULT_RETRY_SLEEP_SEC 30u

static unsigned int retry_sleep_seconds(int attempt_index) {
    (void)attempt_index; /* fixed backoff: same delay every attempt */
    return HU_LORA_DEFAULT_RETRY_SLEEP_SEC;
}

/* ── helpers ────────────────────────────────────────────────────────── */

static bool path_exists_and_nonempty(const char *path) {
    if (!path || !*path)
        return false;
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return st.st_size > 0;
}

static bool ensure_dir_exists(const char *path) {
    if (!path || !*path)
        return false;
    struct stat st;
    if (stat(path, &st) == 0)
        return (st.st_mode & S_IFDIR) != 0;
    return mkdir(path, 0700) == 0;
}

/* ── preflight ─────────────────────────────────────────────────────── */

bool hu_lora_subprocess_preflight_ok(const hu_lora_subprocess_config_t *cfg) {
    if (!cfg)
        return false;
    if (!cfg->base_model[0])
        return false;
    if (!path_exists_and_nonempty(cfg->data_jsonl_path))
        return false;
    if (!ensure_dir_exists(cfg->adapter_output_dir))
        return false;
    /* hu_mlx_lm_module_available returns false under HU_IS_TEST, so
     * unit tests can exercise the rest of preflight without erroring
     * here. The full e2e check fires on production builds. */
    if (!hu_mlx_lm_module_available())
        return false;
    return true;
}

/* ── argv builder ──────────────────────────────────────────────────── */

/* Push one argv entry into out[*out_idx] backed by arg_buf storage.
 * Returns false on overflow. */
static bool argv_push(const char **out, size_t out_cap, size_t *out_idx, char *arg_buf,
                      size_t arg_buf_cap, size_t *arg_buf_off, const char *src) {
    if (*out_idx + 1 >= out_cap)
        return false; /* leave room for the trailing NULL */
    size_t src_len = strlen(src);
    if (*arg_buf_off + src_len + 1 > arg_buf_cap)
        return false;
    char *dst = arg_buf + *arg_buf_off;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    out[(*out_idx)++] = dst;
    *arg_buf_off += src_len + 1;
    return true;
}

size_t hu_lora_subprocess_build_argv(const hu_lora_subprocess_config_t *cfg, const char **argv_out,
                                     size_t argv_cap, char *arg_buf, size_t arg_buf_cap) {
    if (!cfg || !argv_out || argv_cap < 4 || !arg_buf || arg_buf_cap < 64)
        return 0;
    if (!cfg->base_model[0] || !cfg->data_jsonl_path[0] || !cfg->adapter_output_dir[0])
        return 0;

    size_t idx = 0;
    size_t off = 0;
    int batch = cfg->batch_size > 0 ? cfg->batch_size : HU_LORA_DEFAULT_BATCH_SIZE;
    int iters = cfg->iters > 0 ? cfg->iters : HU_LORA_DEFAULT_ITERS;
    int layers = cfg->lora_layers > 0 ? cfg->lora_layers : HU_LORA_DEFAULT_LORA_LAYERS;

    char num_buf[3][16];
    snprintf(num_buf[0], sizeof(num_buf[0]), "%d", batch);
    snprintf(num_buf[1], sizeof(num_buf[1]), "%d", iters);
    snprintf(num_buf[2], sizeof(num_buf[2]), "%d", layers);

    /* Wire format:
     *   python3 -m mlx_lm.lora --model <id> --train
     *     --data <jsonl> --batch-size N --iters N --lora-layers N
     *     --adapter-path <dir>
     *
     * This mirrors the M3 runbook's canonical invocation. Older
     * mlx-lm versions use `mlx_lm.lora` as the entry point; newer
     * versions can be invoked as `mlx_lm.lora` directly if exposed
     * on PATH. The `python3 -m` form is the most portable. */
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "python3"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "-m"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "mlx_lm.lora"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "--model"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, cfg->base_model))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "--train"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "--data"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, cfg->data_jsonl_path))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "--batch-size"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, num_buf[0]))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "--iters"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, num_buf[1]))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "--lora-layers"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, num_buf[2]))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, "--adapter-path"))
        return 0;
    if (!argv_push(argv_out, argv_cap, &idx, arg_buf, arg_buf_cap, &off, cfg->adapter_output_dir))
        return 0;

    argv_out[idx] = NULL;
    return idx;
}

/* ── end-to-end ─────────────────────────────────────────────────────── */

hu_error_t hu_lora_subprocess_train(hu_allocator_t *alloc, const hu_lora_subprocess_config_t *cfg) {
    if (!alloc || !cfg)
        return HU_ERR_INVALID_ARGUMENT;
    if (!cfg->base_model[0] || !cfg->data_jsonl_path[0] || !cfg->adapter_output_dir[0])
        return HU_ERR_INVALID_ARGUMENT;

    if (!hu_lora_subprocess_preflight_ok(cfg)) {
        hu_log_warn("lora-subprocess", NULL,
                    "preflight failed: missing mlx_lm, data file empty, or output dir "
                    "unwritable — skipping");
        return HU_ERR_NOT_SUPPORTED;
    }

    unsigned int timeout = cfg->timeout_sec > 0 ? cfg->timeout_sec : HU_LORA_DEFAULT_TIMEOUT_SEC;
    int max_retries = cfg->max_retries >= 0 ? cfg->max_retries : HU_LORA_DEFAULT_MAX_RETRIES;

    /* Storage for argv across the loop — re-built each attempt because
     * hu_process_run_with_timeout doesn't take ownership. */
    const char *argv[24];
    char arg_buf[2048];

    hu_error_t last_err = HU_ERR_IO;
    for (int attempt = 0; attempt <= max_retries; attempt++) {
        size_t argc = hu_lora_subprocess_build_argv(cfg, argv, sizeof(argv) / sizeof(argv[0]),
                                                    arg_buf, sizeof(arg_buf));
        if (argc == 0) {
            hu_log_error("lora-subprocess", NULL, "argv build failed (likely buffer overflow)");
            return HU_ERR_INVALID_ARGUMENT;
        }

        hu_log_info("lora-subprocess", NULL,
                    "attempt %d/%d: %s -m mlx_lm.lora --model %s (timeout %us)", attempt + 1,
                    max_retries + 1, argv[0], cfg->base_model, timeout);

        hu_run_result_t res;
        memset(&res, 0, sizeof(res));
        hu_error_t r = hu_process_run_with_timeout(
            alloc, argv, NULL, HU_LORA_SUBPROCESS_MAX_OUTPUT_BYTES, timeout, &res);

        if (r == HU_OK && res.exit_code == 0) {
            /* Subprocess exited 0 — verify the adapter file actually
             * appeared. mlx-lm has been known to exit 0 in edge cases
             * without writing the adapter (e.g. iters=0). */
            char adapter_path[HU_LORA_SUBPROCESS_PATH_MAX + 32];
            snprintf(adapter_path, sizeof(adapter_path), "%s/adapters.safetensors",
                     cfg->adapter_output_dir);
            if (path_exists_and_nonempty(adapter_path)) {
                hu_log_info("lora-subprocess", NULL, "training succeeded: %s", adapter_path);
                hu_run_result_free(alloc, &res);
                return HU_OK;
            }
            hu_log_warn("lora-subprocess", NULL,
                        "subprocess exited 0 but %s missing — treating as failure", adapter_path);
            last_err = HU_ERR_IO;
        } else {
            /* Log the last 512 bytes of stderr to aid diagnosis. */
            const char *tail = res.stderr_buf ? res.stderr_buf : "";
            size_t tail_len = res.stderr_len;
            if (tail_len > 512) {
                tail += tail_len - 512;
                tail_len = 512;
            }
            hu_log_error("lora-subprocess", NULL, "attempt %d/%d failed (err=%d exit=%d): %.*s",
                         attempt + 1, max_retries + 1, (int)r, res.exit_code, (int)tail_len, tail);
            last_err = (r == HU_OK) ? HU_ERR_IO : r;
        }
        hu_run_result_free(alloc, &res);

        if (attempt < max_retries) {
            unsigned int wait_s = retry_sleep_seconds(attempt);
            hu_log_info("lora-subprocess", NULL, "waiting %us before retry...", wait_s);
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
            sleep(wait_s);
#endif
        }
    }

    hu_log_error("lora-subprocess", NULL, "training failed after %d attempt(s)", max_retries + 1);
    return last_err;
}
