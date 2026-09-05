/* src/doctor/check_serving_stability.c — see include/human/doctor/check_ops.h
 *
 * 2026-09-02 00:40–02:58: mlx-server died with SIGSEGV after almost every
 * reply (36 restart banners, 25 crash reports) and nothing alarmed. launchd
 * kept restarting it, /health kept answering between deaths, and `doctor`
 * said "inference ok". This check reads the two artifacts that could not lie
 * that morning: the crash-report directory and `launchctl print`'s run count. */
#include "human/doctor/check_ops.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Defaults: a 24 h window and more than two crash reports is a loop, not a blip. */
#define HU_DOCTOR_SERVING_WINDOW_H    24
#define HU_DOCTOR_SERVING_MAX_CRASHES 2

static char s_reason[512];
static char s_detail[512];

static int64_t field_after(const char *text, const char *key) {
    const char *p = text ? strstr(text, key) : NULL;
    if (!p)
        return INT64_MIN;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '=')
        p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p)
        return INT64_MIN;
    return (int64_t)v;
}

int64_t hu_doctor_launchctl_runs(const char *text) {
    int64_t v = field_after(text, "runs");
    return v == INT64_MIN ? -1 : v;
}

int64_t hu_doctor_launchctl_last_exit(const char *text) {
    return field_after(text, "last exit code");
}

static int count_recent_crashes(const char *dir, const char *prefix, int64_t now,
                                int64_t window_s) {
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d)
        return -1;
    int count = 0;
    size_t plen = prefix ? strlen(prefix) : 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (plen && strncmp(e->d_name, prefix, plen) != 0)
            continue;
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 4, ".ips") != 0)
            continue;
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) == 0 && now - (int64_t)st.st_mtime <= window_s)
            count++;
    }
    closedir(d);
    return count;
}

static hu_doctor_check_result_t run(hu_doctor_check_t *self, void *vctx) {
    (void)self;
    const hu_doctor_serving_stability_ctx_t *ctx = (const hu_doctor_serving_stability_ctx_t *)vctx;
    char dir_buf[512];
    const char *dir = hu_doctor_ops_home_path(ctx ? ctx->crash_dir : NULL, dir_buf, sizeof(dir_buf),
                                              "Library/Logs/DiagnosticReports");
    const char *prefix = (ctx && ctx->crash_prefix) ? ctx->crash_prefix : "Python-";
    int64_t now = (ctx && ctx->now_unix) ? ctx->now_unix : (int64_t)time(NULL);
    int window_h = (ctx && ctx->window_hours) ? ctx->window_hours : HU_DOCTOR_SERVING_WINDOW_H;
    int max_crashes = (ctx && ctx->max_crashes) ? ctx->max_crashes : HU_DOCTOR_SERVING_MAX_CRASHES;

    int crashes = count_recent_crashes(dir, prefix, now, (int64_t)window_h * 3600);
    int64_t runs = hu_doctor_launchctl_runs(ctx ? ctx->launchctl_text : NULL);
    int64_t last_exit = hu_doctor_launchctl_last_exit(ctx ? ctx->launchctl_text : NULL);

    /* The daemon is the second process that can crash-loop under KeepAlive with
     * every other check green (2026-09-04: 8 ASan aborts on the memory-store
     * path, replies dropped, nothing alarmed). Same artifacts, its own prefix. */
    const char *dprefix =
        (ctx && ctx->daemon_crash_prefix) ? ctx->daemon_crash_prefix : "human-daemon-";
    int dcrashes = count_recent_crashes(dir, dprefix, now, (int64_t)window_h * 3600);
    int64_t druns = hu_doctor_launchctl_runs(ctx ? ctx->daemon_launchctl_text : NULL);

    snprintf(s_detail, sizeof(s_detail),
             "{\"crash_reports_%dh\":%d,\"launchd_runs\":%lld,\"last_exit_code\":%s,"
             "\"crash_prefix\":\"%s\",\"max_crashes\":%d,"
             "\"daemon_crash_reports_%dh\":%d,\"daemon_launchd_runs\":%lld,"
             "\"daemon_crash_prefix\":\"%s\"}",
             window_h, crashes, (long long)runs, last_exit == INT64_MIN ? "null" : "0", prefix,
             max_crashes, window_h, dcrashes < 0 ? 0 : dcrashes, (long long)druns, dprefix);
    /* patch the real exit code in (kept simple: rewrite the null slot) */
    if (last_exit != INT64_MIN) {
        char *slot = strstr(s_detail, "\"last_exit_code\":0");
        if (slot) {
            char tail[256];
            snprintf(tail, sizeof(tail), "%s", slot + strlen("\"last_exit_code\":0"));
            snprintf(slot, sizeof(s_detail) - (size_t)(slot - s_detail),
                     "\"last_exit_code\":%lld%s", (long long)last_exit, tail);
        }
    }

    if (crashes < 0 && runs < 0) {
        snprintf(s_reason, sizeof(s_reason),
                 "no crash-report directory and no launchd status available — cannot assess");
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason, s_detail};
    }
    char last_exit_str[24] = "n/a";
    if (last_exit != INT64_MIN)
        snprintf(last_exit_str, sizeof(last_exit_str), "%lld", (long long)last_exit);
    if (crashes > max_crashes) {
        snprintf(s_reason, sizeof(s_reason),
                 "local model server is crash-looping: %d %s*.ips crash reports in the last %dh "
                 "(limit %d), launchd runs=%lld last_exit=%s — every restart reloads the model "
                 "and drops live replies; check ~/.human/logs/mlx-server-launchd.log",
                 crashes, prefix, window_h, max_crashes, (long long)runs, last_exit_str);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    if (dcrashes > max_crashes) {
        snprintf(s_reason, sizeof(s_reason),
                 "the daemon itself is crash-looping: %d %s*.ips crash reports in the last %dh "
                 "(limit %d), launchd runs=%lld — launchd restarts it silently and the reply "
                 "that triggered each crash is lost; read ~/.human/logs/asan.log.* and "
                 "~/Library/Logs/DiagnosticReports/%s*.ips",
                 dcrashes, dprefix, window_h, max_crashes, (long long)druns, dprefix);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    if (last_exit != INT64_MIN && last_exit != 0 && last_exit != -15 && last_exit != 15) {
        snprintf(s_reason, sizeof(s_reason),
                 "local model server's last exit was abnormal (code %lld; -11/11 = SIGSEGV) with "
                 "%d crash reports in %dh — watch for a loop",
                 (long long)last_exit, crashes < 0 ? 0 : crashes, window_h);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    snprintf(s_reason, sizeof(s_reason),
             "%d crash report(s) in %dh, launchd runs=%lld; daemon: %d crash report(s), runs=%lld",
             crashes < 0 ? 0 : crashes, window_h, (long long)runs, dcrashes < 0 ? 0 : dcrashes,
             (long long)druns);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
}

const hu_doctor_check_t hu_doctor_check_serving_stability = {
    .name = "serving_stability",
    .description = "Local model server is not crash-looping (crash reports + launchd run count)",
    .run = run,
    .fix = NULL,
    .user_data = NULL,
};
