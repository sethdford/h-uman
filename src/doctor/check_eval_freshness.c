/* src/doctor/check_eval_freshness.c — see include/human/doctor/check_ops.h
 *
 * Two questions, answered separately, because on 2026-09-04 they had
 * different answers: the product gate RAN at 04:13 (multi-turn crashed in
 * its judge, fidelity DEFERRED, blind A/B SKIPPED) and this check said
 * "ran 0.0 days ago". The log and the archive advance on every invocation,
 * so mtime proves invocation, not measurement. PASS now requires a real
 * verdict artifact inside the window. */
#include "human/doctor/check_ops.h"

#include "human/core/allocator.h"
#include "human/core/json.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Default: the gate is nightly; three missed nights is a dark instrument. */
#define HU_DOCTOR_EVAL_MAX_AGE_DAYS 3
/* Verdict files are a few KB; anything larger is not a verdict. */
#define HU_DOCTOR_EVAL_VERDICT_MAX_BYTES 65536

static char s_reason[512];
static char s_detail[384];

static bool has_suffix(const char *name, const char *suffix) {
    size_t n = strlen(name), sn = strlen(suffix);
    return n >= sn && strcmp(name + n - sn, suffix) == 0;
}

static int64_t newest_mtime(const char *dir, const char *suffix) {
    int64_t best = 0;
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_suffix(e->d_name, suffix))
            continue;
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0 && (int64_t)st.st_mtime > best)
            best = (int64_t)st.st_mtime;
    }
    closedir(d);
    return best;
}

static int64_t file_mtime(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0 || st.st_size == 0)
        return 0;
    return (int64_t)st.st_mtime;
}

int64_t hu_doctor_eval_newest_artifact_unix(const char *archive_dir, const char *nightly_log) {
    int64_t a = newest_mtime(archive_dir, ".json");
    int64_t l = file_mtime(nightly_log);
    return a > l ? a : l;
}

bool hu_doctor_eval_verdict_is_real(const char *json, size_t len) {
    if (!json || len == 0)
        return false;
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    if (hu_json_parse(&alloc, json, len, &root) != HU_OK || !root)
        return false;
    bool real = false;
    if (root->type == HU_JSON_OBJECT) {
        const char *v = hu_json_get_string(root, "verdict");
        if (v && v[0]) {
            real = strcmp(v, "DEFERRED") != 0 && strcmp(v, "SKIP") != 0 &&
                   strcmp(v, "SKIPPED") != 0 && strcmp(v, "ERROR") != 0;
        } else if (hu_json_object_get(root, "run_passed")) {
            real = true; /* multiturn completed and judged, PASS or FAIL */
        }
    }
    hu_json_free(&alloc, root);
    return real;
}

/* Read up to HU_DOCTOR_EVAL_VERDICT_MAX_BYTES of `path`; -1 when unreadable. */
static long read_small_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

int64_t hu_doctor_eval_newest_verdict_unix(const char *archive_dir) {
    int64_t best = 0;
    DIR *d = archive_dir ? opendir(archive_dir) : NULL;
    if (!d)
        return 0;
    char *buf = (char *)malloc(HU_DOCTOR_EVAL_VERDICT_MAX_BYTES);
    if (!buf) {
        closedir(d);
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "eval-", 5) != 0 || !has_suffix(e->d_name, ".json") ||
            strstr(e->d_name, "-smoke-"))
            continue;
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/%s", archive_dir, e->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= 0 || (int64_t)st.st_mtime <= best)
            continue;
        long n = read_small_file(path, buf, HU_DOCTOR_EVAL_VERDICT_MAX_BYTES);
        if (n > 0 && hu_doctor_eval_verdict_is_real(buf, (size_t)n))
            best = (int64_t)st.st_mtime;
    }
    free(buf);
    closedir(d);
    return best;
}

static hu_doctor_check_result_t run(hu_doctor_check_t *self, void *vctx) {
    (void)self;
    const hu_doctor_eval_freshness_ctx_t *ctx = (const hu_doctor_eval_freshness_ctx_t *)vctx;
    char archive_buf[512], log_buf[512];
    const char *archive = hu_doctor_ops_home_path(ctx ? ctx->archive_dir : NULL, archive_buf,
                                                  sizeof(archive_buf), ".human/logs/eval-archive");
    const char *log = hu_doctor_ops_home_path(ctx ? ctx->nightly_log : NULL, log_buf,
                                              sizeof(log_buf), ".human/logs/nightly-eval.log");
    int64_t now = (ctx && ctx->now_unix) ? ctx->now_unix : (int64_t)time(NULL);
    int max_days = (ctx && ctx->max_age_days) ? ctx->max_age_days : HU_DOCTOR_EVAL_MAX_AGE_DAYS;

    int64_t last_ran = hu_doctor_eval_newest_artifact_unix(archive, log);
    if (last_ran == 0) {
        snprintf(s_reason, sizeof(s_reason),
                 "product gate (eval-nightly) has NO artifact: neither %s/*.json nor %s exists — "
                 "the blind-A/B/fidelity/multiturn gate has never run here",
                 archive ? archive : "(archive)", log ? log : "(log)");
        snprintf(
            s_detail, sizeof(s_detail),
            "{\"last_run_unix\":0,\"last_verdict_unix\":0,\"age_days\":null,\"max_age_days\":%d}",
            max_days);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    int64_t last_verdict = hu_doctor_eval_newest_verdict_unix(archive);
    double ran_days = (double)(now - last_ran) / 86400.0;
    double verdict_days = last_verdict ? (double)(now - last_verdict) / 86400.0 : -1.0;
    snprintf(s_detail, sizeof(s_detail),
             "{\"last_run_unix\":%lld,\"last_verdict_unix\":%lld,\"age_days\":%.1f,"
             "\"verdict_age_days\":%.1f,\"max_age_days\":%d}",
             (long long)last_ran, (long long)last_verdict, ran_days, verdict_days, max_days);
    if (ran_days > (double)max_days) {
        snprintf(s_reason, sizeof(s_reason),
                 "product gate (eval-nightly) last ran %.1f days ago (limit %d) — its launchd job "
                 "is calendar-only and does not catch up after sleep; run scripts/nightly_eval.sh "
                 "or let scripts/nightly-watchdog.sh drive it",
                 ran_days, max_days);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    if (last_verdict == 0 || verdict_days > (double)max_days) {
        if (last_verdict == 0)
            snprintf(s_reason, sizeof(s_reason),
                     "product gate ran %.1f days ago but has NEVER produced a real verdict — every "
                     "archived run is DEFERRED/SKIPPED or crashed before judging; read %s",
                     ran_days, log ? log : "(log)");
        else
            snprintf(s_reason, sizeof(s_reason),
                     "product gate ran %.1f days ago but its last REAL verdict is %.1f days old "
                     "(limit %d) — recent runs deferred, skipped, or crashed; read %s",
                     ran_days, verdict_days, max_days, log ? log : "(log)");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    snprintf(s_reason, sizeof(s_reason),
             "product gate ran %.1f days ago; last real verdict %.1f days ago", ran_days,
             verdict_days);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
}

const hu_doctor_check_t hu_doctor_check_eval_freshness = {
    .name = "eval_freshness",
    .description = "Product gate (eval-nightly: multiturn/fidelity/blind-A/B) has produced a real "
                   "verdict recently",
    .run = run,
    .fix = NULL,
    .user_data = NULL,
};
