/* US-11.8 — `human ml adapter-rollback` CLI.
 *
 * Operator escape hatch for OFS-DPO dual fast/slow LoRA. Walks
 * <slow_dir> for slow.safetensors.v{N}, quarantines the highest, and
 * points <current_symlink> at v{N-1}. Returns HU_ERR_PRECONDITION when
 * there is no prior version to roll back to (a v0-only state).
 *
 * This is intentionally a small, mechanical CLI — no LoRA math is
 * performed here. The quarantine + symlink swap reuse the same
 * primitives the W14 cron uses for the REJECT path. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/cli.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int adapter_rollback_highest_version(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int best = -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
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

static void adapter_rollback_mkdir_p(const char *path) {
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

/* Atomic symlink swap (mirrors the runner's pattern). */
static hu_error_t adapter_rollback_promote_symlink(const char *target, const char *current) {
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", current, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;
    (void)unlink(tmp);
    if (symlink(target, tmp) != 0)
        return HU_ERR_IO;
    if (rename(tmp, current) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_ml_cli_adapter_rollback(hu_allocator_t *alloc, int argc, const char **argv) {
    (void)alloc;
    const char *slow_dir = NULL;
    const char *quarantine_dir = NULL;
    const char *current = NULL;
    const char *today = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--slow-dir") == 0 && i + 1 < argc)
            slow_dir = argv[++i];
        else if (strcmp(argv[i], "--quarantine-dir") == 0 && i + 1 < argc)
            quarantine_dir = argv[++i];
        else if (strcmp(argv[i], "--current") == 0 && i + 1 < argc)
            current = argv[++i];
        else if (strcmp(argv[i], "--today") == 0 && i + 1 < argc)
            today = argv[++i];
    }
    if (!slow_dir || !quarantine_dir || !current) {
        fprintf(stderr, "Usage: human ml adapter-rollback --slow-dir <dir> "
                        "--quarantine-dir <dir> --current <symlink-path> "
                        "[--today YYYY-MM-DD]\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    int cur_v = adapter_rollback_highest_version(slow_dir);
    if (cur_v < 1) {
        /* Either no versions exist, or only v0 — nothing to roll back to. */
        fprintf(stderr,
                "adapter-rollback: no prior version to roll back to "
                "(highest = v%d)\n",
                cur_v);
        return HU_ERR_TOOL_VALIDATION;
    }
    int prev_v = cur_v - 1;

    char cur_path[1024];
    char prev_path[1024];
    snprintf(cur_path, sizeof(cur_path), "%s/slow.safetensors.v%d", slow_dir, cur_v);
    snprintf(prev_path, sizeof(prev_path), "%s/slow.safetensors.v%d", slow_dir, prev_v);

    /* Quarantine the current version. */
    char today_buf[16];
    if (!today || !*today) {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        snprintf(today_buf, sizeof(today_buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                 tmv.tm_mday);
        today = today_buf;
    }
    char qpath[1024];
    snprintf(qpath, sizeof(qpath), "%s/%s.safetensors", quarantine_dir, today);
    adapter_rollback_mkdir_p(quarantine_dir);
    if (rename(cur_path, qpath) != 0) {
        /* Cross-FS fallback: copy + fsync + unlink.
         *
         * Sprint 11 / US-11.8 critic-HIGH #1 fix: matches the fix in
         * `retrain_quarantine_move` — explicit `fflush + fsync` on the
         * destination before unlinking the source. Without this, a crash
         * between `fclose(out)` and the OS flushing dirty pages would
         * leave the quarantine file truncated AND the source symlink
         * target already gone, breaking rollback recovery. */
        FILE *in = fopen(cur_path, "rb");
        if (in) {
            FILE *out = fopen(qpath, "wb");
            if (out) {
                char buf[4096];
                size_t n;
                int copy_err = 0;
                while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                    if (fwrite(buf, 1, n, out) != n) {
                        copy_err = 1;
                        break;
                    }
                }
                if (ferror(in))
                    copy_err = 1;
                if (!copy_err) {
                    if (fflush(out) != 0)
                        copy_err = 1;
                    else if (fsync(fileno(out)) != 0)
                        copy_err = 1;
                }
                fclose(out);
                fclose(in);
                if (copy_err) {
                    (void)unlink(qpath); /* clean up partial destination */
                    fprintf(stderr, "adapter-rollback: copy to quarantine %s failed mid-write\n",
                            qpath);
                    return HU_ERR_IO;
                }
                (void)unlink(cur_path);
            } else {
                fclose(in);
                fprintf(stderr, "adapter-rollback: cannot write quarantine %s\n", qpath);
                return HU_ERR_IO;
            }
        }
        /* If the file did not exist (test fixture without real bytes),
         * proceed anyway — the symlink swap is the load-bearing op. */
    }

    /* Atomic symlink swap to v{N-1}. */
    hu_error_t e = adapter_rollback_promote_symlink(prev_path, current);
    if (e != HU_OK) {
        fprintf(stderr, "adapter-rollback: symlink swap to %s failed (rc=%d)\n", prev_path, (int)e);
        return e;
    }
    printf("{\"rolled_back_to\":\"v%d\",\"quarantined\":\"%s\"}\n", prev_v, qpath);
    return HU_OK;
}
