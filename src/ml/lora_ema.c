/* US-11.8 — OFS-DPO dual fast/slow LoRA EMA helper (C surface).
 *
 * The numerical heavy lifting (safetensors load/save, per-tensor weighted
 * average) lives in `scripts/lora_ema.py`. This C surface:
 *
 *   1. Validates that paths are well-formed.
 *   2. Detects cold-start (slow_path_in missing) and emits a file copy
 *      via the same Python helper using `--cold-start` so we maintain a
 *      single safetensors writer.
 *   3. Spawns the helper via the runner's subprocess seam — under
 *      `HU_IS_TEST` no real Python ever runs (the runner's exec is
 *      permanently disabled in that build; tests register their own
 *      capture hook).
 *   4. Parses `{"ok": true|false, "reason": "<...>"}` from stdout. On
 *      `ok=false` the reason is propagated up as HU_ERR_PRECONDITION
 *      (compat mismatch) or HU_ERR_IO (other failure).
 *
 * Compat check (rank / target modules / base model) is implemented inside
 * the Python helper because that's where the safetensors metadata is
 * actually readable. The C side records the reason and surfaces it; it
 * does NOT attempt to silently truncate or zero-pad.
 */

#include "human/ml/lora_ema.h"
#include "human/core/error.h"
#include "human/core/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── stdout JSON shred helpers ────────────────────────────────────────── */

/* Returns 1 iff the JSON contains literal `"ok":true`. */
static int lora_ema_stdout_ok(const char *json) {
    if (!json)
        return 0;
    const char *p = strstr(json, "\"ok\"");
    if (!p)
        return 0;
    const char *colon = strchr(p, ':');
    if (!colon)
        return 0;
    /* Skip whitespace */
    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        ++v;
    return strncmp(v, "true", 4) == 0;
}

/* Best-effort extract of `"reason": "<...>"`. Returns 1 iff found and
 * fits in `dest`. */
static int lora_ema_stdout_reason(const char *json, char *dest, size_t cap) {
    if (!json || !dest || cap == 0)
        return 0;
    const char *p = strstr(json, "\"reason\"");
    if (!p)
        return 0;
    const char *colon = strchr(p, ':');
    if (!colon)
        return 0;
    const char *q = strchr(colon, '"');
    if (!q)
        return 0;
    const char *qe = strchr(q + 1, '"');
    if (!qe)
        return 0;
    size_t n = (size_t)(qe - (q + 1));
    if (n >= cap)
        n = cap - 1;
    memcpy(dest, q + 1, n);
    dest[n] = '\0';
    return 1;
}

/* Detect "compat" failure vs "other IO" failure by looking for the
 * compat sentinel strings in the reason. */
static int lora_ema_reason_is_compat(const char *reason) {
    if (!reason || !*reason)
        return 0;
    return (strstr(reason, "rank_mismatch") != NULL ||
            strstr(reason, "target_modules_mismatch") != NULL ||
            strstr(reason, "base_model_mismatch") != NULL ||
            strstr(reason, "shape_mismatch") != NULL);
}

/* ── Filesystem helpers ───────────────────────────────────────────────── */

static int lora_ema_file_exists(const char *path) {
    if (!path || !*path)
        return 0;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ── Public entry: EMA apply ──────────────────────────────────────────── */

hu_error_t hu_lora_ema_apply(hu_lora_ema_ctx_t *ctx) {
    if (!ctx || !ctx->fast_path || !*ctx->fast_path || !ctx->slow_path_out || !*ctx->slow_path_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ctx->run_subprocess)
        return HU_ERR_INVALID_ARGUMENT;
    if (ctx->alpha < 0.0 || ctx->alpha > 1.0) {
        snprintf(ctx->out_reason, sizeof(ctx->out_reason), "alpha_out_of_range:%.3f", ctx->alpha);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *script = ctx->script_path ? ctx->script_path : "scripts/lora_ema.py";
    char alpha_buf[32];
    snprintf(alpha_buf, sizeof(alpha_buf), "%.6f", ctx->alpha);

    ctx->out_was_cold_start = 0;
    ctx->out_reason[0] = '\0';

    /* Cold start: slow_path_in is missing/empty or file does not exist. */
    int is_cold_start =
        (!ctx->slow_path_in || !*ctx->slow_path_in || !lora_ema_file_exists(ctx->slow_path_in));

    hu_lora_retrain_proc_result_t result;
    memset(&result, 0, sizeof(result));

    if (is_cold_start) {
        const char *argv[] = {script,  "--cold-start",     "--fast", ctx->fast_path,
                              "--out", ctx->slow_path_out, NULL};
        hu_error_t e = ctx->run_subprocess(argv, &result, ctx->run_subprocess_ud);
        if (e != HU_OK) {
            snprintf(ctx->out_reason, sizeof(ctx->out_reason), "subprocess_error:%d", (int)e);
            return HU_ERR_IO;
        }
        if (result.exit_code != 0 || !lora_ema_stdout_ok(result.stdout_buf)) {
            (void)lora_ema_stdout_reason(result.stdout_buf, ctx->out_reason,
                                         sizeof(ctx->out_reason));
            return HU_ERR_IO;
        }
        ctx->out_was_cold_start = 1;
        return HU_OK;
    }

    /* Warm path: alpha * slow + (1 - alpha) * fast. */
    const char *argv[] = {
        script,    "--slow",  ctx->slow_path_in, "--fast",           ctx->fast_path,
        "--alpha", alpha_buf, "--out",           ctx->slow_path_out, NULL};
    hu_error_t e = ctx->run_subprocess(argv, &result, ctx->run_subprocess_ud);
    if (e != HU_OK) {
        snprintf(ctx->out_reason, sizeof(ctx->out_reason), "subprocess_error:%d", (int)e);
        return HU_ERR_IO;
    }
    if (result.exit_code != 0 || !lora_ema_stdout_ok(result.stdout_buf)) {
        (void)lora_ema_stdout_reason(result.stdout_buf, ctx->out_reason, sizeof(ctx->out_reason));
        /* Distinguish compat failures from IO failures for upstream
         * outcome routing. */
        if (lora_ema_reason_is_compat(ctx->out_reason))
            return HU_ERR_TOOL_VALIDATION;
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* ── Public entry: KL drift compute ───────────────────────────────────── */

/* Parses `{"kl_nats": <float>}` from stdout. Returns 1 on success. */
static int lora_ema_parse_kl(const char *json, double *out_kl) {
    if (!json || !out_kl)
        return 0;
    const char *p = strstr(json, "\"kl_nats\"");
    if (!p)
        return 0;
    const char *colon = strchr(p, ':');
    if (!colon)
        return 0;
    char *end = NULL;
    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        ++v;
    double d = strtod(v, &end);
    if (end == v)
        return 0;
    *out_kl = d;
    return 1;
}

/* Sprint 11 / US-11.8 critic-CRITICAL #1: detect `"source": "stub"` in the
 * KL JSON output so the C runner can distinguish "real measurement returned
 * 0.0 nats" (genuine, clean PASS) from "torch unavailable, gate not run"
 * (must be flagged to operator, must NOT silently pass the gate). Returns 1
 * iff a `"source": "stub"` literal is present in the JSON. */
static int lora_ema_parse_kl_is_stub(const char *json) {
    if (!json)
        return 0;
    const char *p = strstr(json, "\"source\"");
    if (!p)
        return 0;
    const char *colon = strchr(p, ':');
    if (!colon)
        return 0;
    const char *q = strchr(colon, '"');
    if (!q)
        return 0;
    const char *qe = strchr(q + 1, '"');
    if (!qe)
        return 0;
    size_t n = (size_t)(qe - (q + 1));
    return (n == 4 && memcmp(q + 1, "stub", 4) == 0) ? 1 : 0;
}

hu_error_t hu_lora_compute_kl_drift(const char *base_path, const char *candidate_path,
                                    const char *probe_set_path, const char *script_path,
                                    hu_lora_ema_subprocess_fn run_subprocess, void *ud,
                                    double *out_kl_nats, int *out_is_stub) {
    if (!candidate_path || !*candidate_path || !probe_set_path || !*probe_set_path ||
        !run_subprocess || !out_kl_nats)
        return HU_ERR_INVALID_ARGUMENT;
    if (out_is_stub)
        *out_is_stub = 0;
    const char *script = script_path ? script_path : "scripts/compute_kl_drift.py";
    const char *base = (base_path && *base_path) ? base_path : "";
    const char *argv[] = {script,         "--base",      base,           "--candidate",
                          candidate_path, "--probe-set", probe_set_path, NULL};
    hu_lora_retrain_proc_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t e = run_subprocess(argv, &result, ud);
    if (e != HU_OK)
        return HU_ERR_IO;
    if (result.exit_code != 0)
        return HU_ERR_IO;
    if (!lora_ema_parse_kl(result.stdout_buf, out_kl_nats))
        return HU_ERR_IO;
    if (out_is_stub)
        *out_is_stub = lora_ema_parse_kl_is_stub(result.stdout_buf);
    return HU_OK;
}
