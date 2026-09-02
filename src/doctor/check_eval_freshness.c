/* src/doctor/check_eval_freshness.c — see include/human/doctor/check_ops.h */
#include "human/doctor/check_ops.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Default: the gate is nightly; three missed nights is a dark instrument. */
#define HU_DOCTOR_EVAL_MAX_AGE_DAYS 3

static char s_reason[512];
static char s_detail[384];

static int64_t newest_mtime(const char *dir, const char *suffix) {
    int64_t best = 0;
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name), sn = strlen(suffix);
        if (n < sn || strcmp(e->d_name + n - sn, suffix) != 0)
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

    int64_t last = hu_doctor_eval_newest_artifact_unix(archive, log);
    if (last == 0) {
        snprintf(s_reason, sizeof(s_reason),
                 "product gate (eval-nightly) has NO artifact: neither %s/*.json nor %s exists — "
                 "the blind-A/B/fidelity/multiturn gate has never run here",
                 archive ? archive : "(archive)", log ? log : "(log)");
        snprintf(s_detail, sizeof(s_detail),
                 "{\"last_run_unix\":0,\"age_days\":null,\"max_age_days\":%d}", max_days);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    double age_days = (double)(now - last) / 86400.0;
    snprintf(s_detail, sizeof(s_detail),
             "{\"last_run_unix\":%lld,\"age_days\":%.1f,\"max_age_days\":%d}", (long long)last,
             age_days, max_days);
    if (age_days > (double)max_days) {
        snprintf(s_reason, sizeof(s_reason),
                 "product gate (eval-nightly) last ran %.1f days ago (limit %d) — its launchd job "
                 "is calendar-only and does not catch up after sleep; run scripts/nightly_eval.sh "
                 "or let scripts/nightly-watchdog.sh drive it",
                 age_days, max_days);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    snprintf(s_reason, sizeof(s_reason), "product gate ran %.1f days ago", age_days);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
}

const hu_doctor_check_t hu_doctor_check_eval_freshness = {
    .name = "eval_freshness",
    .description = "Product gate (eval-nightly: multiturn/fidelity/blind-A/B) has run recently",
    .run = run,
    .fix = NULL,
    .user_data = NULL,
};
