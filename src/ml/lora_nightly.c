/* src/ml/lora_nightly.c
 *
 * Nightly export → train → swap orchestrator.
 * Sprint B residuals #3 (2026-05-24). */

#include "human/ml/lora_nightly.h"

#include "human/core/log.h"
#include "human/ml/lora_export.h"
#include "human/ml/lora_subprocess.h"
#include "human/ml/mlx_admin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── config defaults ───────────────────────────────────────────────── */

bool hu_lora_nightly_config_init_defaults(hu_lora_nightly_config_t *cfg) {
    if (!cfg)
        return false;
    memset(cfg, 0, sizeof(*cfg));
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s/.human/memory.db", home);
    snprintf(cfg->pairs_jsonl_path, sizeof(cfg->pairs_jsonl_path), "%s/.human/lora-pairs.jsonl",
             home);
    snprintf(cfg->adapters_dir, sizeof(cfg->adapters_dir), "%s/.human/adapters", home);
    snprintf(cfg->current_symlink, sizeof(cfg->current_symlink), "%s/.human/adapter-current", home);
    snprintf(cfg->mlx_base_url, sizeof(cfg->mlx_base_url), "http://127.0.0.1:8741/v1");
    /* Default base model — same as the M3 runbook example. Users on
     * different hardware (more/less VRAM) should override via the
     * struct or via the future daemon config block. */
    snprintf(cfg->base_model, sizeof(cfg->base_model), "mlx-community/gemma-2-2b-it-4bit");
    cfg->dry_run = false;
    return true;
}

/* ── pure predicate ─────────────────────────────────────────────────── */

bool hu_lora_nightly_should_run(int64_t now_unix, int64_t last_run_unix, int32_t new_pairs_since) {
    if (new_pairs_since < HU_LORA_NIGHTLY_MIN_NEW_PAIRS)
        return false;
    if (last_run_unix == 0)
        return true;
    return (now_unix - last_run_unix) >= HU_LORA_NIGHTLY_MIN_INTERVAL_SEC;
}

/* ── atomic symlink rotation ────────────────────────────────────────── */

hu_error_t hu_lora_nightly_rotate_symlink(const char *current_symlink, const char *target_dir) {
    if (!current_symlink || !*current_symlink || !target_dir || !*target_dir)
        return HU_ERR_INVALID_ARGUMENT;
    /* Stage to a temp path adjacent to the destination so rename is
     * atomic-on-same-fs. We don't fsync the symlink (POSIX doesn't
     * promise sync semantics for symlink rename), but rename is
     * crash-consistent on common filesystems. */
    char tmp_path[HU_LORA_NIGHTLY_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", current_symlink, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return HU_ERR_IO;
    /* Pre-clean any stale tmp link from a prior crashed run. */
    (void)unlink(tmp_path);
    if (symlink(target_dir, tmp_path) != 0) {
        /* errno=EEXIST shouldn't happen after the unlink, but be defensive. */
        return HU_ERR_IO;
    }
    /* rename(tmp, dst) is atomic on POSIX even when dst already exists. */
    if (rename(tmp_path, current_symlink) != 0) {
        (void)unlink(tmp_path);
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* ── pick next adapter version dir ──────────────────────────────────── */

/* Walk `adapters_dir/v*` and pick a free v<N>. Returns false on
 * error. Caller-owned buffer. */
static bool pick_next_version_dir(const char *adapters_dir, char *out, size_t cap) {
    if (!adapters_dir || !*adapters_dir || !out || cap < 16)
        return false;
    /* Ensure adapters_dir exists. */
    (void)mkdir(adapters_dir, 0700);
    /* Linear probe; we don't expect >1000 versions in any realistic
     * deployment. If we somehow do, the caller can prune. */
    for (int v = 1; v < 10000; v++) {
        int n = snprintf(out, cap, "%s/v%d", adapters_dir, v);
        if (n < 0 || (size_t)n >= cap)
            return false;
        struct stat st;
        if (stat(out, &st) != 0 && errno == ENOENT)
            return true;
    }
    return false;
}

/* ── end-to-end orchestrator ────────────────────────────────────────── */

hu_error_t hu_lora_nightly_run(hu_allocator_t *alloc, const hu_lora_nightly_config_t *cfg,
                               int64_t now_unix, size_t *out_pair_count) {
    if (!alloc || !cfg)
        return HU_ERR_INVALID_ARGUMENT;
    if (out_pair_count)
        *out_pair_count = 0;

    /* Step 1 — export. The window is "all pairs" by passing since=0;
     * the policy gate above already filters by min-new-pairs. */
    size_t count = 0;
    hu_error_t err =
        hu_lora_export_dpo_pairs(alloc, cfg->db_path, cfg->pairs_jsonl_path, 0, &count);
    if (err == HU_ERR_NOT_SUPPORTED) {
        hu_log_info("lora-nightly", NULL, "export not supported on this build — skipping nightly");
        return err;
    }
    if (err != HU_OK) {
        hu_log_error("lora-nightly", NULL, "export failed: %d", (int)err);
        return err;
    }
    if (out_pair_count)
        *out_pair_count = count;
    if (count == 0) {
        hu_log_info("lora-nightly", NULL, "no pairs to train on — skipping");
        return HU_ERR_NOT_FOUND;
    }
    hu_log_info("lora-nightly", NULL, "exported %zu pairs to %s", count, cfg->pairs_jsonl_path);

    /* Step 2 — pick the next version dir. */
    char next_dir[HU_LORA_NIGHTLY_PATH_MAX];
    if (!pick_next_version_dir(cfg->adapters_dir, next_dir, sizeof(next_dir))) {
        hu_log_error("lora-nightly", NULL, "could not allocate next adapter version dir under %s",
                     cfg->adapters_dir);
        return HU_ERR_IO;
    }

    /* Step 3 — train.
     *
     * Either delegate to hu_lora_subprocess_train (N1, wired
     * 2026-05-25) OR skip in dry-run mode for smoke-testing the
     * rotation+swap layers without invoking mlx_lm.
     *
     * Create the version directory FIRST in both branches so the
     * subprocess has somewhere to write, and so dry-run mode produces
     * a valid empty dir that rotation can point at. */
    if (mkdir(next_dir, 0700) != 0 && errno != EEXIST) {
        hu_log_error("lora-nightly", NULL, "mkdir %s failed: %s", next_dir, strerror(errno));
        return HU_ERR_IO;
    }

    if (cfg->dry_run) {
        hu_log_info("lora-nightly", NULL, "dry-run mode: skipping mlx_lm.lora subprocess");
    } else if (!cfg->base_model[0]) {
        hu_log_warn("lora-nightly", NULL,
                    "base_model not configured; skipping subprocess (rotation + swap continue "
                    "against empty dir)");
    } else {
        hu_lora_subprocess_config_t sp;
        memset(&sp, 0, sizeof(sp));
        snprintf(sp.base_model, sizeof(sp.base_model), "%s", cfg->base_model);
        snprintf(sp.data_jsonl_path, sizeof(sp.data_jsonl_path), "%s", cfg->pairs_jsonl_path);
        snprintf(sp.adapter_output_dir, sizeof(sp.adapter_output_dir), "%s", next_dir);
        /* batch_size/iters/lora_layers/timeout/retries = 0 → use
         * lora_subprocess.c's USER-CONFIRMED defaults (30min, 1 retry,
         * 30s backoff, M3-runbook hyperparameters). */
        hu_error_t te = hu_lora_subprocess_train(alloc, &sp);
        if (te == HU_ERR_NOT_SUPPORTED) {
            hu_log_warn("lora-nightly", NULL,
                        "subprocess preflight failed (mlx-lm not installed?); skipping training "
                        "but continuing rotation+swap for diagnostic visibility");
        } else if (te != HU_OK) {
            hu_log_error("lora-nightly", NULL,
                         "subprocess training failed (%d); skipping rotation+swap this run",
                         (int)te);
            return te;
        }
    }

    /* Step 4 — atomic rotation of the symlink. */
    hu_error_t r = hu_lora_nightly_rotate_symlink(cfg->current_symlink, next_dir);
    if (r != HU_OK) {
        hu_log_error("lora-nightly", NULL, "symlink rotation %s -> %s failed", cfg->current_symlink,
                     next_dir);
        return r;
    }
    hu_log_info("lora-nightly", NULL, "rotated %s -> %s", cfg->current_symlink, next_dir);

    /* Step 5 — live swap on the running MLX server. The adapter path
     * we send is the symlink target (so future rotations don't change
     * what's loaded until the next swap fires). */
    char adapter_file[HU_LORA_NIGHTLY_PATH_MAX];
    int an = snprintf(adapter_file, sizeof(adapter_file), "%s/adapters.safetensors", next_dir);
    if (an < 0 || (size_t)an >= sizeof(adapter_file))
        return HU_ERR_IO;

    hu_mlx_admin_swap_result_t res;
    memset(&res, 0, sizeof(res));
    hu_error_t se = hu_mlx_admin_swap_adapter(alloc, cfg->mlx_base_url, strlen(cfg->mlx_base_url),
                                              adapter_file, strlen(adapter_file), &res);
    if (se != HU_OK) {
        /* Expected in dry-run mode (the adapter file doesn't exist).
         * Log and return OK anyway — the export + rotation succeeded. */
        hu_log_info("lora-nightly", NULL,
                    "live swap to %s returned %d (expected in dry-run; check MLX server log)",
                    adapter_file, (int)se);
    } else {
        hu_log_info("lora-nightly", NULL, "live swap to %s OK", adapter_file);
    }
    hu_mlx_admin_swap_result_free(alloc, &res);
    (void)now_unix; /* reserved for future "last-run" persistence */

    return HU_OK;
}
