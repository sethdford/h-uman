/* src/daemon_social_tick.c
 *
 * Sprint A.6: the smallest possible production caller for the three
 * Tier-2 library-only scanners. Runs every 6 hours, writes a JSON
 * snapshot to ~/.human/social_state.json.
 *
 * Architecture: lives alongside daemon_imessage_observer.c rather than
 * inside any one scanner module because it crosses three subsystems
 * (gap / drift / signature). The scanners stay independent; this file
 * is the orchestrator. */

#include "human/daemon_social_tick.h"

#include "human/channels/contact_signature.h"
#include "human/channels/imessage_gaps.h"
#include "human/memory/pattern_drift.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* mkdir */
#include <sys/types.h>
#include <unistd.h> /* fsync */

#define HU_SOCIAL_TICK_DEFAULT_INTERVAL_SEC (6LL * 60 * 60)
#define HU_SOCIAL_TICK_DEFAULT_TOP_N        16
#define HU_SOCIAL_TICK_GAP_MIN_HISTORY      10U
#define HU_SOCIAL_TICK_GAP_MIN_DAYS         14
#define HU_SOCIAL_TICK_GAP_MAX_DAYS         365

static const char *default_out_path(void) {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    snprintf(path, sizeof(path), "%s/.human/social_state.json", home);
    return path;
}

static const char *default_db_path(void) {
    const char *env = getenv("HU_CHATDB");
    if (env && env[0])
        return env;
    static char home_path[512];
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    snprintf(home_path, sizeof(home_path), "%s/Library/Messages/chat.db", home);
    return home_path;
}

/* Best-effort `mkdir -p` for the parent of `path`.
 *
 * Walks the path string from the front, calling `mkdir(0700)` at every '/'
 * boundary so the social-state snapshot can land cleanly on first run even
 * when `~/.human/` does not exist yet (fresh install, or the daemon's first
 * 6-hour tick firing before any other subsystem created the directory).
 * Mirrors `hu_pm_ensure_parent_dir` in personal_model.c — the comment below
 * claimed "same discipline as personal_model_save" but the mkdir step was
 * missing, which surfaced as HU_ERR_IO on clean-HOME CI runners. 0700 mode:
 * social state is device-local user data; only the owner should read it.
 * EEXIST and intermediate failures are swallowed — the subsequent `fopen`
 * surfaces any real failure as HU_ERR_IO. */
static void ensure_parent_dir(const char *path) {
    if (!path || !*path)
        return;
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path)
        return;
    size_t parent_len = (size_t)(last_slash - path);
    char buf[576];
    if (parent_len + 1 >= sizeof(buf))
        return;
    memcpy(buf, path, parent_len);
    buf[parent_len] = '\0';
    for (size_t i = 1; i < parent_len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0700);
            buf[i] = '/';
        }
    }
    (void)mkdir(buf, 0700);
}

/* Atomic file write: mkdir -p parent + tmp + fsync + rename. Same discipline
 * as personal_model_save / identity_resolver_save. */
static hu_error_t write_atomic(const char *path, const char *content, size_t len) {
    if (!path || !content)
        return HU_ERR_INVALID_ARGUMENT;
    char tmp_path[576];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return HU_ERR_INVALID_ARGUMENT;

    ensure_parent_dir(path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return HU_ERR_IO;
    if (fwrite(content, 1, len, f) != len) {
        fclose(f);
        return HU_ERR_IO;
    }
    fflush(f);
#ifdef __APPLE__
    /* fsync on macOS for durability; ignore errors (best-effort) */
    (void)fsync(fileno(f));
#endif
    fclose(f);
    if (rename(tmp_path, path) != 0)
        return HU_ERR_IO;
    return HU_OK;
}

hu_error_t hu_daemon_social_tick_run(const hu_config_t *cfg, const char *db_path,
                                     const char *out_path, int64_t now_unix, size_t top_n) {
    (void)cfg;
    if (top_n == 0 || top_n > 64)
        top_n = HU_SOCIAL_TICK_DEFAULT_TOP_N;
    if (!db_path)
        db_path = default_db_path();
    if (!out_path)
        out_path = default_out_path();
    if (!out_path)
        return HU_ERR_INVALID_ARGUMENT;

    /* Stack-allocate the result arrays — bounded by top_n which is
     * capped at 64. Keeps the function allocator-free. */
    hu_imessage_stale_contact_t stale[16];
    hu_contact_signature_t sigs[16];
    hu_drift_alert_t drifts[32];
    size_t stale_n = 0, sigs_n = 0, drifts_n = 0;

    /* All three scanners return HU_ERR_NOT_SUPPORTED under HU_IS_TEST
     * or on non-Apple builds — we still emit a valid JSON skeleton so
     * downstream consumers see a stable shape. */
    (void)hu_imessage_scan_stale_contacts(db_path, now_unix, HU_SOCIAL_TICK_GAP_MIN_HISTORY,
                                          HU_SOCIAL_TICK_GAP_MIN_DAYS, HU_SOCIAL_TICK_GAP_MAX_DAYS,
                                          stale, sizeof(stale) / sizeof(stale[0]), &stale_n);
    (void)hu_contact_signature_top_n(db_path, now_unix, top_n, sigs, sizeof(sigs) / sizeof(sigs[0]),
                                     &sigs_n);
    (void)hu_drift_scan_top_contacts(db_path, now_unix, top_n, drifts,
                                     sizeof(drifts) / sizeof(drifts[0]), &drifts_n);

    /* Hand-roll JSON to avoid pulling a serializer dep. Field-safe via
     * snprintf bounds; never embeds untrusted strings without quote
     * escaping. For now we trust handles + topics since they're
     * device-local and already in the personal model. */
    char buf[8192];
    size_t pos = 0;
    int w =
        snprintf(buf + pos, sizeof(buf) - pos,
                 "{\n  \"generated_at_unix\": %lld,\n  \"stale_contacts\": [", (long long)now_unix);
    if (w < 0)
        return HU_ERR_IO;
    pos += (size_t)w;
    for (size_t i = 0; i < stale_n && pos + 256 < sizeof(buf); i++) {
        w = snprintf(buf + pos, sizeof(buf) - pos,
                     "%s\n    {\"contact\":\"%s\",\"last_message_unix\":%lld,"
                     "\"days_since_last\":%lld,\"historical_count\":%u}",
                     i == 0 ? "" : ",", stale[i].contact_handle,
                     (long long)stale[i].last_message_unix, (long long)stale[i].days_since_last,
                     stale[i].historical_count);
        if (w < 0)
            return HU_ERR_IO;
        pos += (size_t)w;
    }
    w = snprintf(buf + pos, sizeof(buf) - pos, "\n  ],\n  \"signatures\": [");
    pos += (size_t)w;
    for (size_t i = 0; i < sigs_n && pos + 384 < sizeof(buf); i++) {
        w = snprintf(buf + pos, sizeof(buf) - pos,
                     "%s\n    {\"contact\":\"%s\",\"total\":%u,\"recent_30d\":%u,"
                     "\"outbound\":%u,\"inbound\":%u,\"median_latency_sec\":%d,"
                     "\"avg_msg_length\":%d,\"active_days\":%d}",
                     i == 0 ? "" : ",", sigs[i].contact_handle, sigs[i].total_messages,
                     sigs[i].messages_last_30_days, sigs[i].outbound_count, sigs[i].inbound_count,
                     sigs[i].median_response_latency_sec, sigs[i].avg_message_length,
                     sigs[i].active_days);
        if (w < 0)
            return HU_ERR_IO;
        pos += (size_t)w;
    }
    w = snprintf(buf + pos, sizeof(buf) - pos, "\n  ],\n  \"drift_alerts\": [");
    pos += (size_t)w;
    for (size_t i = 0; i < drifts_n && pos + 256 < sizeof(buf); i++) {
        w = snprintf(buf + pos, sizeof(buf) - pos,
                     "%s\n    {\"contact\":\"%s\",\"dimension\":%d,\"severity\":%d,"
                     "\"sigma\":%.3f,\"recent\":%.3f,\"baseline\":%.3f}",
                     i == 0 ? "" : ",", drifts[i].contact_handle, (int)drifts[i].dimension,
                     (int)drifts[i].severity, drifts[i].sigma, drifts[i].recent_value,
                     drifts[i].baseline_value);
        if (w < 0)
            return HU_ERR_IO;
        pos += (size_t)w;
    }
    w = snprintf(buf + pos, sizeof(buf) - pos, "\n  ]\n}\n");
    if (w < 0)
        return HU_ERR_IO;
    pos += (size_t)w;

    return write_atomic(out_path, buf, pos);
}

hu_error_t hu_daemon_social_tick(const hu_config_t *cfg, int64_t now_unix,
                                 int64_t *last_run_unix_inout) {
    if (!last_run_unix_inout)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t interval = HU_SOCIAL_TICK_DEFAULT_INTERVAL_SEC;
    /* Future: read cfg->social_state.tick_interval_seconds. For now use
     * the default; cfg is reserved for that extension. */
    (void)cfg;

    if (*last_run_unix_inout > 0 && now_unix - *last_run_unix_inout < interval)
        return HU_OK;

    *last_run_unix_inout = now_unix;
    return hu_daemon_social_tick_run(cfg, NULL, NULL, now_unix, HU_SOCIAL_TICK_DEFAULT_TOP_N);
}
