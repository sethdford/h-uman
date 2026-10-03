/* src/doctor/check_adapter_disk.c — see include/human/doctor/check_ops.h
 *
 * 2026-10-03: the nightly retrain's adapter output dir filled the disk
 * because nothing pruned old candidates. scripts/retrain/prune_adapters.py
 * is the fix; this check is the operator-visible signal that the pruner
 * needs to run (or needs to be flipped from shadow to live). */
#include "human/core/paths.h"
#include "human/doctor/check_ops.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static char s_reason[384];
static char s_detail[256];

static bool hu_adapter_prune_is_nightly_family(const char *name) {
    return strncmp(name, "seth-m3-outcomes-", 17) == 0 ||
           strncmp(name, "seth-glm-air-mlxtune-", 21) == 0;
}

int64_t hu_doctor_dir_size_bytes(const char *path) {
    if (!path)
        return 0;
    struct stat top;
    if (lstat(path, &top) != 0)
        return 0;
    if (S_ISLNK(top.st_mode))
        return 0; /* never follow a symlink into or out of the tree */
    if (!S_ISDIR(top.st_mode))
        return (int64_t)top.st_size;

    int64_t total = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[2048];
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child))
            continue;
        struct stat st;
        if (lstat(child, &st) != 0 || S_ISLNK(st.st_mode))
            continue;
        if (S_ISDIR(st.st_mode))
            total += hu_doctor_dir_size_bytes(child); /* bounded by real FS depth */
        else if (S_ISREG(st.st_mode))
            total += (int64_t)st.st_size;
    }
    closedir(d);
    return total;
}

static int64_t sum_nightly_candidates(const char *adapters_dir) {
    DIR *d = adapters_dir ? opendir(adapters_dir) : NULL;
    if (!d)
        return 0;
    int64_t total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!hu_adapter_prune_is_nightly_family(e->d_name))
            continue;
        char child[2048];
        if (snprintf(child, sizeof(child), "%s/%s", adapters_dir, e->d_name) >= (int)sizeof(child))
            continue;
        struct stat st;
        if (lstat(child, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
            continue;
        total += hu_doctor_dir_size_bytes(child);
    }
    closedir(d);
    return total;
}

bool hu_doctor_adapter_disk_should_fail(int64_t free_gb, int64_t candidate_gb, int64_t warn_free_gb,
                                        int64_t error_free_gb, int64_t candidate_warn_gb) {
    if (free_gb < error_free_gb)
        return true;
    if (free_gb < warn_free_gb)
        return true;
    if (candidate_gb > candidate_warn_gb)
        return true;
    return false;
}

static hu_doctor_check_result_t run(hu_doctor_check_t *self, void *vctx) {
    (void)self;
    const hu_doctor_adapter_disk_ctx_t *ctx = (const hu_doctor_adapter_disk_ctx_t *)vctx;

    char dirbuf[512];
    const char *adapters_dir = ctx ? ctx->adapters_dir : NULL;
    if (!adapters_dir) {
        if (hu_paths_state(dirbuf, sizeof(dirbuf), "training-data/adapters") < 0)
            return (hu_doctor_check_result_t){HU_DOCTOR_NA,
                                              "cannot resolve the state directory (no $HOME / "
                                              "$HU_STATE_DIR)",
                                              NULL};
        adapters_dir = dirbuf;
    }

    int64_t warn_free_gb = (ctx && ctx->warn_free_gb) ? ctx->warn_free_gb : 50;
    int64_t error_free_gb = (ctx && ctx->error_free_gb) ? ctx->error_free_gb : 10;
    int64_t candidate_warn_gb = (ctx && ctx->candidate_warn_gb) ? ctx->candidate_warn_gb : 20;

    int64_t free_bytes = (ctx && ctx->free_bytes >= 0) ? ctx->free_bytes : -1;
    if (free_bytes < 0) {
        struct statvfs sv;
        free_bytes =
            (statvfs(adapters_dir, &sv) == 0) ? (int64_t)sv.f_bavail * (int64_t)sv.f_frsize : -1;
    }
    if (free_bytes < 0) {
        snprintf(s_reason, sizeof(s_reason), "cannot statvfs %s", adapters_dir);
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason, NULL};
    }

    int64_t candidate_bytes = (ctx && ctx->candidate_bytes >= 0) ? ctx->candidate_bytes : -1;
    if (candidate_bytes < 0)
        candidate_bytes = sum_nightly_candidates(adapters_dir);

    double free_gb = (double)free_bytes / (1024.0 * 1024.0 * 1024.0);
    double candidate_gb = (double)candidate_bytes / (1024.0 * 1024.0 * 1024.0);

    snprintf(s_detail, sizeof(s_detail),
             "{\"free_gb\":%.1f,\"candidate_gb\":%.1f,\"warn_free_gb\":%lld,"
             "\"error_free_gb\":%lld,\"candidate_warn_gb\":%lld}",
             free_gb, candidate_gb, (long long)warn_free_gb, (long long)error_free_gb,
             (long long)candidate_warn_gb);

    bool fail = hu_doctor_adapter_disk_should_fail((int64_t)free_gb, (int64_t)candidate_gb,
                                                   warn_free_gb, error_free_gb, candidate_warn_gb);
    if (fail) {
        snprintf(
            s_reason, sizeof(s_reason),
            "%.1f GB free (%s<%lld GB) and %.1f GB of nightly adapter candidates in %s — "
            "run HU_ADAPTER_PRUNE=live scripts/retrain/prune_adapters.py (see the shadow "
            "log first, then docs/guides/persona-adapter-retrain-runbook.md#adapter-retention)",
            free_gb, free_gb < (double)error_free_gb ? "CRITICAL " : "", (long long)warn_free_gb,
            candidate_gb, adapters_dir);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    snprintf(s_reason, sizeof(s_reason), "%.1f GB free, %.1f GB of nightly adapter candidates",
             free_gb, candidate_gb);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
}

const hu_doctor_check_t hu_doctor_check_adapter_disk = {
    .name = "adapter_disk",
    .description = "Nightly retrain adapter output is pruned and the disk has headroom",
    .run = run,
    .fix = NULL,
    .user_data = NULL,
};
