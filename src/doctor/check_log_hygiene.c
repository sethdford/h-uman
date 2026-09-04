/* src/doctor/check_log_hygiene.c — see include/human/doctor/check_ops.h */
#include "human/doctor/check_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static char s_reason[512];
static char s_detail[256];

bool hu_doctor_log_size(const char *path, int64_t *out_bytes) {
    struct stat st;
    if (!path || !out_bytes || stat(path, &st) != 0)
        return false;
    *out_bytes = (int64_t)st.st_size;
    return true;
}

static hu_doctor_check_result_t run(hu_doctor_check_t *self, void *vctx) {
    (void)self;
    const hu_doctor_log_hygiene_ctx_t *ctx = (const hu_doctor_log_hygiene_ctx_t *)vctx;
    char buf[512];
    const char *path = hu_doctor_ops_home_path(ctx ? ctx->log_path : NULL, buf, sizeof(buf),
                                               ".human/logs/service-loop-error.log");
    int64_t max = (ctx && ctx->max_bytes) ? ctx->max_bytes : (int64_t)50 * 1024 * 1024;
    int64_t bytes = 0;
    if (!hu_doctor_log_size(path, &bytes)) {
        snprintf(s_reason, sizeof(s_reason), "daemon log not present (%s)", path ? path : "?");
        snprintf(s_detail, sizeof(s_detail), "{\"bytes\":null,\"max_bytes\":%lld}", (long long)max);
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason, s_detail};
    }
    snprintf(s_detail, sizeof(s_detail), "{\"bytes\":%lld,\"max_bytes\":%lld}", (long long)bytes,
             (long long)max);
    if (bytes > max) {
        snprintf(s_reason, sizeof(s_reason),
                 "daemon log is %.1f MB (limit %.0f MB) and launchd appends forever — "
                 "run scripts/rotate-logs.sh (the nightly watchdog schedules it)",
                 (double)bytes / 1048576.0, (double)max / 1048576.0);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    snprintf(s_reason, sizeof(s_reason), "daemon log %.1f MB", (double)bytes / 1048576.0);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
}

const hu_doctor_check_t hu_doctor_check_log_hygiene = {
    .name = "log_hygiene",
    .description = "Daemon log is bounded (rotation is running)",
    .run = run,
    .fix = NULL,
    .user_data = NULL,
};
