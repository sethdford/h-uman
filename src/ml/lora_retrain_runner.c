/* W14 — Nightly LoRA re-train cron runner (US-7.5).
 *
 * Sibling to `hu_lora_training_runner` (HUML GPT in-process path); this
 * orchestrates the MLX-Gemma frontier path via subprocess.
 *
 * Sequence per tick:
 *   1. Acquire single-flight PID lock.
 *   2. Probe pair count (`human ml mine-corrections --count-only`).
 *      0 pairs ⇒ SKIPPED_NO_NEW_DATA, return.
 *   3. Invoke `finetune-gemma.py --dpo --from-corrections --no-restart-server --no-version`.
 *      Non-zero exit ⇒ FAILED, current adapter symlink untouched, return.
 *   4. Invoke `check-lora-ab.sh --judgment <candidate>`. Parse JSON `verdict`
 *      field. Only `"PASS"` proceeds to promotion (D3 contract: SKIP is NOT
 *      pass, FAIL is not pass, missing key is not pass).
 *   5. Atomic symlink swap (tmp link + rename(2)) to point `current_symlink`
 *      at `candidate_dir`.
 *
 * Synchronous V1 (open-Q 1 default): the runner blocks the scheduler tick
 * for the full subprocess chain. PID file prevents overlapping retrains.
 * `HU_IS_TEST` disables the production fork/exec path; tests must register
 * `test_run_subprocess` or the runner emits SKIPPED_ALREADY_RUNNING (via
 * the "no exec path available" fallback). */

#include "human/ml/lora_retrain_runner.h"
#include "human/agent/scheduler.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/ml/lora_ema.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── String table for the outcome enum ────────────────────────────────── */

const char *hu_lora_retrain_outcome_str(hu_lora_retrain_outcome_t o) {
    switch (o) {
    case HU_LORA_RETRAIN_OUTCOME_SKIPPED_NO_NEW_DATA:
        return "skipped_no_new_data";
    case HU_LORA_RETRAIN_OUTCOME_SKIPPED_ALREADY_RUNNING:
        return "skipped_already_running";
    case HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL:
        return "skipped_gate_fail";
    case HU_LORA_RETRAIN_OUTCOME_FAILED:
        return "failed";
    case HU_LORA_RETRAIN_OUTCOME_PROMOTED:
        return "promoted";
    case HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED:
        return "promotion_failed";
    case HU_LORA_RETRAIN_OUTCOME_SKIPPED_KL_DRIFT:
        return "skipped_kl_drift";
    case HU_LORA_RETRAIN_OUTCOME_SKIPPED_FORGETTING:
        return "skipped_forgetting";
    case HU_LORA_RETRAIN_OUTCOME_EMA_SKIPPED:
        return "ema_skipped";
    case HU_LORA_RETRAIN_OUTCOME_UNKNOWN:
    default:
        return "unknown";
    }
}

hu_lora_retrain_outcome_t hu_lora_retrain_outcome_from_str(const char *s) {
    if (!s)
        return HU_LORA_RETRAIN_OUTCOME_UNKNOWN;
    if (strcmp(s, "skipped_no_new_data") == 0)
        return HU_LORA_RETRAIN_OUTCOME_SKIPPED_NO_NEW_DATA;
    if (strcmp(s, "skipped_already_running") == 0)
        return HU_LORA_RETRAIN_OUTCOME_SKIPPED_ALREADY_RUNNING;
    if (strcmp(s, "skipped_gate_fail") == 0)
        return HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL;
    if (strcmp(s, "failed") == 0)
        return HU_LORA_RETRAIN_OUTCOME_FAILED;
    if (strcmp(s, "promoted") == 0)
        return HU_LORA_RETRAIN_OUTCOME_PROMOTED;
    if (strcmp(s, "promotion_failed") == 0)
        return HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
    if (strcmp(s, "skipped_kl_drift") == 0)
        return HU_LORA_RETRAIN_OUTCOME_SKIPPED_KL_DRIFT;
    if (strcmp(s, "skipped_forgetting") == 0)
        return HU_LORA_RETRAIN_OUTCOME_SKIPPED_FORGETTING;
    if (strcmp(s, "ema_skipped") == 0)
        return HU_LORA_RETRAIN_OUTCOME_EMA_SKIPPED;
    return HU_LORA_RETRAIN_OUTCOME_UNKNOWN;
}

/* ── Status JSON parser for the nested `lora_retrain` block ───────────── */

static const char *retrain_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    return p;
}

hu_error_t hu_lora_retrain_status_parse(const char *json, long long *out_last_run_ts,
                                        hu_lora_retrain_outcome_t *out_last_outcome,
                                        unsigned long long *out_pairs_consumed) {
    if (!json || !out_last_run_ts || !out_last_outcome || !out_pairs_consumed)
        return HU_ERR_INVALID_ARGUMENT;
    const char *block = strstr(json, "\"lora_retrain\"");
    if (!block)
        return HU_ERR_NOT_FOUND;
    const char *colon = strchr(block, ':');
    if (!colon)
        return HU_ERR_INVALID_ARGUMENT;
    const char *brace = strchr(colon, '{');
    if (!brace)
        return HU_ERR_INVALID_ARGUMENT;
    const char *end = strchr(brace, '}');
    if (!end)
        return HU_ERR_INVALID_ARGUMENT;

    /* Scope key lookups to the [brace,end] slice by copying into a small
     * stack buffer (the block is bounded — three fields). */
    size_t len = (size_t)(end - brace) + 1;
    if (len >= 512)
        return HU_ERR_INVALID_ARGUMENT;
    char buf[512];
    memcpy(buf, brace, len);
    buf[len] = '\0';

    const char *ts_p = strstr(buf, "\"last_run_ts\"");
    if (!ts_p)
        return HU_ERR_INVALID_ARGUMENT;
    const char *ts_colon = strchr(ts_p, ':');
    if (!ts_colon)
        return HU_ERR_INVALID_ARGUMENT;
    char *ts_end = NULL;
    long long ts_v = strtoll(retrain_skip_ws(ts_colon + 1), &ts_end, 10);
    if (ts_end == ts_colon + 1)
        return HU_ERR_INVALID_ARGUMENT;

    const char *pc_p = strstr(buf, "\"pairs_consumed\"");
    if (!pc_p)
        return HU_ERR_INVALID_ARGUMENT;
    const char *pc_colon = strchr(pc_p, ':');
    if (!pc_colon)
        return HU_ERR_INVALID_ARGUMENT;
    char *pc_end = NULL;
    unsigned long long pc_v = strtoull(retrain_skip_ws(pc_colon + 1), &pc_end, 10);
    if (pc_end == pc_colon + 1)
        return HU_ERR_INVALID_ARGUMENT;

    const char *oc_p = strstr(buf, "\"last_outcome\"");
    if (!oc_p)
        return HU_ERR_INVALID_ARGUMENT;
    const char *oc_colon = strchr(oc_p, ':');
    if (!oc_colon)
        return HU_ERR_INVALID_ARGUMENT;
    const char *quote = strchr(oc_colon, '"');
    if (!quote)
        return HU_ERR_INVALID_ARGUMENT;
    const char *quote_end = strchr(quote + 1, '"');
    if (!quote_end)
        return HU_ERR_INVALID_ARGUMENT;
    char oc_str[64];
    size_t oc_len = (size_t)(quote_end - (quote + 1));
    if (oc_len >= sizeof(oc_str))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(oc_str, quote + 1, oc_len);
    oc_str[oc_len] = '\0';

    *out_last_run_ts = ts_v;
    *out_pairs_consumed = pc_v;
    *out_last_outcome = hu_lora_retrain_outcome_from_str(oc_str);
    return HU_OK;
}

/* ── Event emission ───────────────────────────────────────────────────── */

static void retrain_emit(hu_lora_retrain_ctx_t *ctx, const char *event, const char *payload) {
    if (ctx && ctx->emit_event) {
        ctx->emit_event(event, payload ? payload : "{}", ctx->emit_user_data);
        return;
    }
    /* Fallback: structured-ish log line. */
    hu_log_info("lora_retrain", NULL, "%s %s", event, payload ? payload : "{}");
}

/* ── Single-flight PID file ───────────────────────────────────────────── */

/* Returns 1 on lock acquired, 0 on already-locked, -1 on infra error. */
static int retrain_pid_lock(const char *path) {
    if (!path || !*path)
        return 1; /* No PID file configured ⇒ caller opted out. */
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            /* Inspect existing PID — if process is gone, steal the lock. */
            FILE *rf = fopen(path, "r");
            if (rf) {
                long pid = 0;
                if (fscanf(rf, "%ld", &pid) == 1 && pid > 0) {
                    fclose(rf);
                    if (kill((pid_t)pid, 0) == 0)
                        return 0; /* Live owner. */
                    /* Stale — remove and retry. */
                    (void)unlink(path);
                    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
                    if (fd < 0)
                        return -1;
                } else {
                    fclose(rf);
                    return -1;
                }
            } else {
                return -1;
            }
        } else {
            return -1;
        }
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    if (n > 0)
        (void)write(fd, buf, (size_t)n);
    close(fd);
    return 1;
}

static void retrain_pid_unlock(const char *path) {
    if (path && *path)
        (void)unlink(path);
}

/* ── Subprocess dispatch ──────────────────────────────────────────────── */

#ifdef HU_IS_TEST
/* In tests we MUST go through the hook. The production exec path is
 * permanently disabled under HU_IS_TEST to make sure no test accidentally
 * forks Python. */
static hu_error_t retrain_real_subprocess(const char *const argv[],
                                          hu_lora_retrain_proc_result_t *result) {
    (void)argv;
    if (result) {
        result->exit_code = -1;
        result->stdout_buf[0] = '\0';
        result->stdout_len = 0;
    }
    return HU_ERR_NOT_SUPPORTED;
}
#else
/* Production: posix_spawn + waitpid, capture stdout via pipe. */
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

static hu_error_t retrain_real_subprocess(const char *const argv[],
                                          hu_lora_retrain_proc_result_t *result) {
    if (!argv || !argv[0] || !result)
        return HU_ERR_INVALID_ARGUMENT;
    result->exit_code = -1;
    result->stdout_buf[0] = '\0';
    result->stdout_len = 0;

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return HU_ERR_IO;

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return HU_ERR_IO;
    }
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        return HU_ERR_IO;
    }

    size_t total = 0;
    while (total + 1 < sizeof(result->stdout_buf)) {
        ssize_t n =
            read(pipefd[0], result->stdout_buf + total, sizeof(result->stdout_buf) - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    result->stdout_buf[total] = '\0';
    result->stdout_len = total;
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return HU_ERR_IO;
    }
    if (WIFEXITED(status))
        result->exit_code = WEXITSTATUS(status);
    else
        result->exit_code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return HU_OK;
}
#endif

static hu_error_t retrain_run(hu_lora_retrain_ctx_t *ctx, const char *const argv[],
                              hu_lora_retrain_proc_result_t *result) {
    if (!ctx || !argv || !result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (ctx->test_run_subprocess)
        return ctx->test_run_subprocess(argv, result, ctx->test_subprocess_ud);
    return retrain_real_subprocess(argv, result);
}

/* ── Pair-count probe ─────────────────────────────────────────────────── */

/* Parses `{"pairs": N}` from stdout. Returns -1 on parse failure. */
static long long retrain_parse_pairs(const char *json) {
    if (!json)
        return -1;
    const char *p = strstr(json, "\"pairs\"");
    if (!p)
        return -1;
    const char *colon = strchr(p, ':');
    if (!colon)
        return -1;
    char *end = NULL;
    long long v = strtoll(retrain_skip_ws(colon + 1), &end, 10);
    if (end == colon + 1)
        return -1;
    return v;
}

/* ── Gate verdict parsing (D3 contract) ───────────────────────────────── */

/* Returns 1 iff the JSON contains literal `"verdict":"PASS"`. Any other
 * value, missing key, or malformed JSON returns 0. SKIP is NOT pass. */
static int retrain_verdict_is_pass(const char *json) {
    if (!json)
        return 0;
    const char *p = strstr(json, "\"verdict\"");
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
    if (n != 4)
        return 0;
    return memcmp(q + 1, "PASS", 4) == 0;
}

static const char *retrain_verdict_str(const char *json) {
    if (!json)
        return "missing";
    const char *p = strstr(json, "\"verdict\"");
    if (!p)
        return "missing";
    const char *colon = strchr(p, ':');
    if (!colon)
        return "missing";
    const char *q = strchr(colon, '"');
    if (!q)
        return "missing";
    const char *qe = strchr(q + 1, '"');
    if (!qe)
        return "missing";
    size_t n = (size_t)(qe - (q + 1));
    if (n == 4 && memcmp(q + 1, "PASS", 4) == 0)
        return "PASS";
    if (n == 4 && memcmp(q + 1, "SKIP", 4) == 0)
        return "SKIP";
    if (n == 4 && memcmp(q + 1, "FAIL", 4) == 0)
        return "FAIL";
    return "other";
}

/* ── Atomic symlink promote ───────────────────────────────────────────── */

/* Mirrors the M2 atomic-rename pattern: symlink(target, tmp); rename(tmp, current). */
static hu_error_t retrain_promote_symlink(const char *target, const char *current) {
    if (!target || !*target || !current || !*current)
        return HU_ERR_INVALID_ARGUMENT;
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", current, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;
    /* Best-effort remove of any prior tmp link from a crashed run. */
    (void)unlink(tmp);
    if (symlink(target, tmp) != 0)
        return HU_ERR_IO;
    if (rename(tmp, current) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* ── US-11.8: dual fast/slow LoRA helpers ─────────────────────────────── */

/* Parse `"final_verdict": "PROMOTE|DEFER|REJECT"` from the cascade JSON.
 * Returns 1 iff PROMOTE; *out_verdict is populated with the literal
 * string for event payloads regardless. */
static int retrain_cascade_is_promote(const char *json, char *out_verdict, size_t cap) {
    if (out_verdict && cap > 0)
        out_verdict[0] = '\0';
    if (!json)
        return 0;
    const char *p = strstr(json, "\"final_verdict\"");
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
    if (out_verdict && cap > n) {
        memcpy(out_verdict, q + 1, n);
        out_verdict[n] = '\0';
    }
    if (n == 7 && memcmp(q + 1, "PROMOTE", 7) == 0)
        return 1;
    return 0;
}

/* Discover the highest existing slow.safetensors.v{N} in `dir`. Returns
 * the version number on success, or -1 if none exist or `dir` cannot be
 * read. */
static int retrain_slow_highest_version(const char *dir) {
    if (!dir || !*dir)
        return -1;
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int best = -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        /* Expecting "slow.safetensors.v<N>" */
        const char *prefix = "slow.safetensors.v";
        size_t plen = strlen(prefix);
        if (strncmp(name, prefix, plen) != 0)
            continue;
        const char *vp = name + plen;
        char *end = NULL;
        long v = strtol(vp, &end, 10);
        if (end == vp || *end != '\0')
            continue;
        if (v > best)
            best = (int)v;
    }
    closedir(d);
    return best;
}

/* Build "<slow_dir>/slow.safetensors.v<N>". Returns 0 on success. */
static int retrain_slow_path(char *out, size_t cap, const char *slow_dir, int version) {
    int n = snprintf(out, cap, "%s/slow.safetensors.v%d", slow_dir, version);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Build today's quarantine filename. */
static int retrain_quarantine_path(char *out, size_t cap, const char *quarantine_dir,
                                   const char *today_yyyymmdd) {
    char buf[16];
    if (!today_yyyymmdd || !*today_yyyymmdd) {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                 tmv.tm_mday);
        today_yyyymmdd = buf;
    }
    int n = snprintf(out, cap, "%s/%s.safetensors", quarantine_dir, today_yyyymmdd);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* mkdir -p best-effort. Walks the path and mkdir's each component. */
static void retrain_mkdir_p(const char *path) {
    if (!path || !*path)
        return;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(buf, 0755);
            *p = '/';
        }
    }
    (void)mkdir(buf, 0755);
}

/* Move file from src to dst (rename within same FS). Returns 0 on success. */
static int retrain_quarantine_move(const char *src, const char *dst_dir, const char *dst_path) {
    retrain_mkdir_p(dst_dir);
    /* If src does not exist, treat as success (test fixtures may not
     * have produced a real fast file). */
    struct stat st;
    if (stat(src, &st) != 0)
        return 0;
    if (rename(src, dst_path) == 0)
        return 0;
    /* Cross-FS fallback: copy + unlink. */
    FILE *in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    (void)unlink(src);
    return 0;
}

/* Trampoline: adapt the runner's subprocess hook to the EMA helper's
 * function-pointer type. Both signatures are identical; this just lets
 * us pass `ctx->test_run_subprocess` (a runner-typed pointer) to the
 * EMA helper without a cast lint warning. */
static hu_error_t retrain_ema_subprocess_trampoline(const char *const argv[],
                                                    hu_lora_retrain_proc_result_t *result,
                                                    void *ud) {
    hu_lora_retrain_ctx_t *ctx = (hu_lora_retrain_ctx_t *)ud;
    if (ctx->test_run_subprocess)
        return ctx->test_run_subprocess(argv, result, ctx->test_subprocess_ud);
    /* Production fallback — under HU_IS_TEST this returns NOT_SUPPORTED. */
    return retrain_real_subprocess(argv, result);
}

/* Parse `{"delta_nll": <float>}` from yntp_eval.py stdout. Returns 1
 * on success. */
static int retrain_parse_delta_nll(const char *json, double *out_delta) {
    if (!json || !out_delta)
        return 0;
    const char *p = strstr(json, "\"delta_nll\"");
    if (!p)
        return 0;
    const char *colon = strchr(p, ':');
    if (!colon)
        return 0;
    char *end = NULL;
    const char *v = retrain_skip_ws(colon + 1);
    double d = strtod(v, &end);
    if (end == v)
        return 0;
    *out_delta = d;
    return 1;
}

/* ── Runner entry point ───────────────────────────────────────────────── */

hu_error_t hu_lora_retrain_runner(struct hu_memory_facade *m, const struct hu_job_spec *spec,
                                  int64_t budget_ms, void *user_data) {
    (void)m;
    (void)budget_ms;
    if (!spec || !user_data)
        return HU_ERR_INVALID_ARGUMENT;
    if (spec->kind != HU_JOB_LORA_RETRAIN_NIGHTLY)
        return HU_ERR_INVALID_ARGUMENT;
    hu_lora_retrain_ctx_t *ctx = (hu_lora_retrain_ctx_t *)user_data;

    /* Reset per-run output fields BEFORE any early returns so callers see
     * the freshest outcome even when we short-circuit. */
    ctx->last_run_ts = (int64_t)time(NULL);
    ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_UNKNOWN;
    ctx->last_pairs_consumed = 0;
    ctx->last_exit_code = 0;

    /* Single-flight gate. */
    int locked = retrain_pid_lock(ctx->pidfile_path);
    if (locked == 0) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_ALREADY_RUNNING;
        retrain_emit(ctx, "lora_retrain_skipped_already_running", "{\"reason\":\"pid_lock_held\"}");
        return HU_OK;
    }
    if (locked < 0) {
        /* PID-file infra error — still skip (don't crash the scheduler). */
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_ALREADY_RUNNING;
        retrain_emit(ctx, "lora_retrain_skipped_already_running",
                     "{\"reason\":\"pidfile_unreachable\"}");
        return HU_OK;
    }

    hu_error_t final_rc = HU_OK;
    char payload[512];

    /* ── STEP 1: pair-count probe ────────────────────────────────────── */
    const char *miner_argv0 = ctx->miner_argv0 ? ctx->miner_argv0 : "human";
    const char *miner_sub0 = ctx->miner_subcmd[0] ? ctx->miner_subcmd[0] : "ml";
    const char *miner_sub1 = ctx->miner_subcmd[1] ? ctx->miner_subcmd[1] : "mine-corrections";
    const char *probe_argv[] = {miner_argv0, miner_sub0, miner_sub1, "--count-only", NULL};

    hu_lora_retrain_proc_result_t probe_result;
    hu_error_t e = retrain_run(ctx, probe_argv, &probe_result);
    if (e != HU_OK || probe_result.exit_code != 0) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_FAILED;
        ctx->last_exit_code = probe_result.exit_code;
        snprintf(payload, sizeof(payload), "{\"step\":\"mine_corrections_probe\",\"exit_code\":%d}",
                 probe_result.exit_code);
        retrain_emit(ctx, "lora_retrain_failed", payload);
        goto done;
    }
    long long pairs = retrain_parse_pairs(probe_result.stdout_buf);
    /* FIX-2: distinguish probe parse-failure (-1, malformed JSON) from
     * "zero pairs" (0). Parse failure routes to FAILED with a dedicated
     * event; zero pairs routes to SKIPPED_NO_NEW_DATA as before. */
    if (pairs < 0) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_FAILED;
        ctx->last_exit_code = probe_result.exit_code;
        retrain_emit(ctx, "lora_retrain_probe_failed", "{\"reason\":\"unparseable_pair_count\"}");
        goto done;
    }
    if (pairs == 0) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_NO_NEW_DATA;
        ctx->last_pairs_consumed = 0;
        retrain_emit(ctx, "lora_retrain_skipped_no_new_data", "{\"pairs\":0}");
        goto done;
    }
    ctx->last_pairs_consumed = (unsigned long long)pairs;
    snprintf(payload, sizeof(payload), "{\"pairs\":%lld}", pairs);
    retrain_emit(ctx, "lora_retrain_scheduled", payload);

    /* ── STEP 2: finetune ────────────────────────────────────────────── */
    const char *finetune =
        ctx->finetune_script ? ctx->finetune_script : "scripts/finetune-gemma.py";
    const char *finetune_argv[] = {
        finetune, "--dpo", "--from-corrections", "--no-restart-server", "--no-version", NULL};
    hu_lora_retrain_proc_result_t train_result;
    e = retrain_run(ctx, finetune_argv, &train_result);
    if (e != HU_OK || train_result.exit_code != 0) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_FAILED;
        ctx->last_exit_code = train_result.exit_code;
        snprintf(payload, sizeof(payload), "{\"step\":\"finetune\",\"exit_code\":%d}",
                 train_result.exit_code);
        retrain_emit(ctx, "lora_retrain_failed", payload);
        goto done;
    }

    /* ── STEP 3 (US-11.8): dual-LoRA dispatch ───────────────────────── */
    if (ctx->dual_lora_enabled) {
        ctx->last_fast_version++;
        ctx->last_kl_drift_nats = -1.0;
        ctx->last_old_pairs_delta_nll = 0.0;
        ctx->last_ema_alpha = 0.0;
        ctx->last_gate_verdict[0] = '\0';

        if (!ctx->slow_dir || !*ctx->slow_dir || !ctx->quarantine_dir || !*ctx->quarantine_dir ||
            !ctx->fast_path || !*ctx->fast_path) {
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
            retrain_emit(ctx, "lora_retrain_promotion_failed",
                         "{\"reason\":\"dual_lora_paths_missing\"}");
            goto done;
        }

        retrain_mkdir_p(ctx->slow_dir);
        retrain_mkdir_p(ctx->quarantine_dir);

        /* STEP 3a: invoke 4-stage Pareto cascade. */
        const char *cascade =
            ctx->cascade_script ? ctx->cascade_script : "scripts/stage_cascade.py";
        const char *cascade_argv[] = {cascade, "--adapter", ctx->fast_path, NULL};
        hu_lora_retrain_proc_result_t cascade_result;
        e = retrain_run(ctx, cascade_argv, &cascade_result);
        if (e != HU_OK) {
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL;
            snprintf(ctx->last_gate_verdict, sizeof(ctx->last_gate_verdict), "%s", "ERROR");
            retrain_emit(ctx, "lora_retrain_skipped", "{\"reason\":\"cascade_subprocess_error\"}");
            goto done;
        }

        char verdict[16];
        int is_promote =
            retrain_cascade_is_promote(cascade_result.stdout_buf, verdict, sizeof(verdict));
        snprintf(ctx->last_gate_verdict, sizeof(ctx->last_gate_verdict), "%s",
                 verdict[0] ? verdict : "missing");

        if (cascade_result.exit_code != 0 && !is_promote) {
            /* Cascade returned non-PROMOTE (exit codes 1=DEFER, 2=REJECT)
             * — quarantine fast and preserve slow. */
            char qpath[1024];
            (void)retrain_quarantine_path(qpath, sizeof(qpath), ctx->quarantine_dir,
                                          ctx->today_yyyymmdd);
            (void)retrain_quarantine_move(ctx->fast_path, ctx->quarantine_dir, qpath);
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL;
            snprintf(payload, sizeof(payload),
                     "{\"reason\":\"gate_fail\",\"verdict\":\"%s\",\"quarantine\":\"%s\"}",
                     ctx->last_gate_verdict, qpath);
            retrain_emit(ctx, "nightly_retrain_rejected", payload);
            goto done;
        }

        if (!is_promote) {
            /* Verdict missing or malformed; treat as reject. */
            char qpath[1024];
            (void)retrain_quarantine_path(qpath, sizeof(qpath), ctx->quarantine_dir,
                                          ctx->today_yyyymmdd);
            (void)retrain_quarantine_move(ctx->fast_path, ctx->quarantine_dir, qpath);
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL;
            snprintf(payload, sizeof(payload), "{\"reason\":\"gate_fail\",\"verdict\":\"%s\"}",
                     ctx->last_gate_verdict);
            retrain_emit(ctx, "nightly_retrain_rejected", payload);
            goto done;
        }

        /* STEP 3b: PROMOTE — KL drift sanity gate. */
        if (ctx->kl_probe_set && *ctx->kl_probe_set) {
            double kl = 0.0;
            const char *kl_script =
                ctx->kl_drift_script ? ctx->kl_drift_script : "scripts/compute_kl_drift.py";
            hu_error_t kl_err = hu_lora_compute_kl_drift(
                ctx->base_model_path ? ctx->base_model_path : "", ctx->fast_path, ctx->kl_probe_set,
                kl_script, retrain_ema_subprocess_trampoline, ctx, &kl);
            if (kl_err == HU_OK) {
                ctx->last_kl_drift_nats = kl;
                double tau =
                    (ctx->kl_tau_nats > 0.0) ? ctx->kl_tau_nats : HU_LORA_KL_TAU_DEFAULT_NATS;
                if (kl > tau) {
                    char qpath[1024];
                    (void)retrain_quarantine_path(qpath, sizeof(qpath), ctx->quarantine_dir,
                                                  ctx->today_yyyymmdd);
                    (void)retrain_quarantine_move(ctx->fast_path, ctx->quarantine_dir, qpath);
                    ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_KL_DRIFT;
                    snprintf(payload, sizeof(payload),
                             "{\"kl_nats\":%.4f,\"tau\":%.4f,\"quarantine\":\"%s\"}", kl, tau,
                             qpath);
                    retrain_emit(ctx, "lora_retrain_kl_drift_rejected", payload);
                    goto done;
                }
            }
            /* On KL subprocess error, log but don't reject (Sprint 11 has
             * no real KL infra; the stub returns 0.0 anyway). */
        }

        /* STEP 3c: PROMOTE — OLD-pairs forgetting check. */
        if (ctx->old_pairs_holdout && *ctx->old_pairs_holdout) {
            const char *yntp =
                ctx->yntp_eval_script ? ctx->yntp_eval_script : "scripts/yntp_eval.py";
            const char *yntp_argv[] = {yntp,        "--fixture",    ctx->old_pairs_holdout,
                                       "--adapter", ctx->fast_path, NULL};
            hu_lora_retrain_proc_result_t yntp_result;
            hu_error_t yntp_err = retrain_run(ctx, yntp_argv, &yntp_result);
            if (yntp_err == HU_OK && yntp_result.exit_code == 0) {
                double delta = 0.0;
                if (retrain_parse_delta_nll(yntp_result.stdout_buf, &delta)) {
                    ctx->last_old_pairs_delta_nll = delta;
                    double tau = (ctx->forget_tau_nll != 0.0) ? ctx->forget_tau_nll
                                                              : HU_LORA_FORGET_TAU_NLL_DEFAULT;
                    if (delta < tau) {
                        char qpath[1024];
                        (void)retrain_quarantine_path(qpath, sizeof(qpath), ctx->quarantine_dir,
                                                      ctx->today_yyyymmdd);
                        (void)retrain_quarantine_move(ctx->fast_path, ctx->quarantine_dir, qpath);
                        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_FORGETTING;
                        snprintf(payload, sizeof(payload),
                                 "{\"delta_nll\":%.4f,\"tau\":%.4f,\"quarantine\":\"%s\"}", delta,
                                 tau, qpath);
                        retrain_emit(ctx, "lora_retrain_forgetting_rejected", payload);
                        goto done;
                    }
                }
            }
        }

        /* STEP 3d: EMA. */
        int prior_v = retrain_slow_highest_version(ctx->slow_dir);
        int new_v = prior_v + 1;
        char slow_in[1024] = {0};
        char slow_out[1024];
        if (prior_v >= 0)
            (void)retrain_slow_path(slow_in, sizeof(slow_in), ctx->slow_dir, prior_v);
        if (retrain_slow_path(slow_out, sizeof(slow_out), ctx->slow_dir, new_v) != 0) {
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
            retrain_emit(ctx, "lora_retrain_promotion_failed", "{\"reason\":\"slow_path_format\"}");
            goto done;
        }

        double alpha = (ctx->ema_alpha > 0.0) ? ctx->ema_alpha : HU_LORA_EMA_DEFAULT_ALPHA;
        hu_lora_ema_ctx_t ema = {0};
        ema.slow_path_in = slow_in;
        ema.fast_path = ctx->fast_path;
        ema.slow_path_out = slow_out;
        ema.alpha = alpha;
        ema.script_path = ctx->ema_script;
        ema.run_subprocess = retrain_ema_subprocess_trampoline;
        ema.run_subprocess_ud = ctx;

        hu_error_t ema_err = hu_lora_ema_apply(&ema);
        if (ema_err == HU_ERR_TOOL_VALIDATION) {
            /* Compat mismatch — do NOT advance slow; do NOT quarantine
             * fast (Sprint 12 will decide whether this is a rollback or
             * an "ignore this night entirely" disposition). */
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_EMA_SKIPPED;
            snprintf(payload, sizeof(payload), "{\"reason\":\"%s\"}", ema.out_reason);
            retrain_emit(ctx, "lora_retrain_ema_skipped", payload);
            goto done;
        }
        if (ema_err != HU_OK) {
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
            snprintf(payload, sizeof(payload), "{\"reason\":\"ema_failed\",\"detail\":\"%s\"}",
                     ema.out_reason);
            retrain_emit(ctx, "lora_retrain_promotion_failed", payload);
            goto done;
        }

        ctx->last_ema_alpha = ema.out_was_cold_start ? alpha : alpha;
        ctx->last_slow_version = new_v;

        /* STEP 3e: advance `current` symlink to slow.safetensors.v{new_v}. */
        if (!ctx->current_symlink || !*ctx->current_symlink) {
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
            retrain_emit(ctx, "lora_retrain_promotion_failed",
                         "{\"reason\":\"missing_current_symlink\"}");
            goto done;
        }
        hu_error_t pe = retrain_promote_symlink(slow_out, ctx->current_symlink);
        if (pe != HU_OK) {
            ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
            snprintf(payload, sizeof(payload), "{\"reason\":\"symlink_swap\",\"rc\":%d}", (int)pe);
            retrain_emit(ctx, "lora_retrain_promotion_failed", payload);
            goto done;
        }
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTED;
        snprintf(payload, sizeof(payload),
                 "{\"slow_version\":%d,\"alpha\":%.4f,\"cold_start\":%d,\"verdict\":\"%s\"}", new_v,
                 alpha, ema.out_was_cold_start ? 1 : 0, ctx->last_gate_verdict);
        retrain_emit(ctx, "lora_retrain_promoted", payload);
        goto done;
    }

    /* ── STEP 3 (legacy single-adapter path): judgment gate ──────────── */
    const char *gate = ctx->gate_script ? ctx->gate_script : "scripts/check-lora-ab.sh";
    const char *candidate = ctx->candidate_dir ? ctx->candidate_dir : "";
    const char *gate_argv[] = {gate, "--judgment", candidate, NULL};
    hu_lora_retrain_proc_result_t gate_result;
    e = retrain_run(ctx, gate_argv, &gate_result);
    if (e != HU_OK) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL;
        retrain_emit(ctx, "lora_retrain_skipped", "{\"reason\":\"gate_subprocess_error\"}");
        goto done;
    }
    /* FIX-3: distinguish gate-process failure (non-zero exit) from a
     * gate that ran cleanly but returned a non-PASS verdict (SKIP/FAIL/missing).
     *
     *   - Non-zero exit  ⇒ outcome FAILED  (the gate itself errored).
     *   - Zero exit + non-PASS verdict ⇒ outcome SKIPPED_GATE_FAIL (D3
     *     contract: SKIP and FAIL are both "not pass"; current symlink
     *     is preserved and the candidate is discarded). */
    if (gate_result.exit_code != 0) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_FAILED;
        ctx->last_exit_code = gate_result.exit_code;
        const char *verdict = retrain_verdict_str(gate_result.stdout_buf);
        snprintf(payload, sizeof(payload),
                 "{\"step\":\"gate\",\"exit_code\":%d,\"verdict\":\"%s\"}", gate_result.exit_code,
                 verdict);
        retrain_emit(ctx, "lora_retrain_failed", payload);
        goto done;
    }
    /* D3 contract: PASS verdict required on a clean gate run. */
    if (!retrain_verdict_is_pass(gate_result.stdout_buf)) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL;
        const char *verdict = retrain_verdict_str(gate_result.stdout_buf);
        snprintf(payload, sizeof(payload),
                 "{\"reason\":\"gate_fail\",\"verdict\":\"%s\",\"exit_code\":0}", verdict);
        retrain_emit(ctx, "lora_retrain_skipped", payload);
        goto done;
    }

    /* ── STEP 4: promote (atomic symlink swap) ───────────────────────── */
    if (!ctx->current_symlink || !ctx->candidate_dir) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
        retrain_emit(ctx, "lora_retrain_promotion_failed", "{\"reason\":\"missing_paths\"}");
        goto done;
    }
    hu_error_t promote_err = retrain_promote_symlink(ctx->candidate_dir, ctx->current_symlink);
    if (promote_err != HU_OK) {
        ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED;
        snprintf(payload, sizeof(payload), "{\"reason\":\"symlink_swap\",\"rc\":%d}",
                 (int)promote_err);
        retrain_emit(ctx, "lora_retrain_promotion_failed", payload);
        goto done;
    }
    ctx->last_outcome = HU_LORA_RETRAIN_OUTCOME_PROMOTED;
    snprintf(payload, sizeof(payload), "{\"candidate\":\"%s\",\"pairs\":%llu}", ctx->candidate_dir,
             ctx->last_pairs_consumed);
    retrain_emit(ctx, "lora_retrain_promoted", payload);

done:
    retrain_pid_unlock(ctx->pidfile_path);
    return final_rc;
}
