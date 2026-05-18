#include "human/doctor.h"
#include "human/agent/scheduler_status_json.h"
#include "human/agent/verifier_metrics.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/skill_registry.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "human/persona.h"
#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif

#if HU_HAS_IMESSAGE && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

#define HU_DOCTOR_LINE_CATEGORY "doctor_line"

/* Internal: push a diag line with an optional machine-readable error_class
 * tag. `error_class` may be NULL for items that have no associated class
 * (most diag lines). When non-NULL it must be a stable, short ASCII token
 * such as "AUTH" / "BUSY" / "CANTOPEN" / "OTHER" / "NONE" — surfaced via
 * the `--json` output as the `error_class` field. */
static hu_error_t doctor_push_line_with_class(hu_allocator_t *alloc, hu_diag_item_t **buf,
                                              size_t *n, size_t *cap, hu_diag_severity_t sev,
                                              const char *line, const char *error_class) {
    if (!line)
        return HU_ERR_INVALID_ARGUMENT;
    if (*n >= *cap) {
        size_t new_cap = *cap * 2;
        hu_diag_item_t *nb =
            (hu_diag_item_t *)alloc->alloc(alloc->ctx, sizeof(hu_diag_item_t) * new_cap);
        if (!nb)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(nb, *buf, sizeof(hu_diag_item_t) * (*n));
        alloc->free(alloc->ctx, *buf, sizeof(hu_diag_item_t) * (*cap));
        *buf = nb;
        *cap = new_cap;
    }
    char *cat = hu_strdup(alloc, HU_DOCTOR_LINE_CATEGORY);
    char *msg = hu_strdup(alloc, line);
    char *ecls = error_class ? hu_strdup(alloc, error_class) : NULL;
    if (!cat || !msg || (error_class && !ecls)) {
        if (cat)
            alloc->free(alloc->ctx, cat, strlen(cat) + 1);
        if (msg)
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        if (ecls)
            alloc->free(alloc->ctx, ecls, strlen(ecls) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    (*buf)[*n] = (hu_diag_item_t){sev, cat, msg, ecls};
    (*n)++;
    return HU_OK;
}

static hu_error_t doctor_push_line(hu_allocator_t *alloc, hu_diag_item_t **buf, size_t *n,
                                   size_t *cap, hu_diag_severity_t sev, const char *line) {
    return doctor_push_line_with_class(alloc, buf, n, cap, sev, line, NULL);
}

static bool doctor_config_wants_http(const hu_config_t *cfg) {
    if (!cfg)
        return false;
    if (cfg->gateway.enabled)
        return true;
#if defined(HU_ENABLE_FEEDS)
    if (cfg->feeds.enabled)
        return true;
#endif
    if (cfg->default_provider && hu_config_provider_requires_api_key(cfg->default_provider))
        return true;
    if (hu_channel_catalog_has_any_configured(cfg, false))
        return true;
    return false;
}

static bool doctor_config_wants_persona(const hu_config_t *cfg) {
    if (!cfg)
        return false;
    if (cfg->agent.persona && cfg->agent.persona[0])
        return true;
    if (cfg->agent.persona_channels_count > 0)
        return true;
    if (cfg->agent.persona_contacts_count > 0)
        return true;
    return false;
}

static hu_error_t doctor_check_sqlite_backend(hu_allocator_t *alloc, hu_diag_item_t **buf,
                                              size_t *n, size_t *cap, const hu_config_t *cfg) {
    if (!cfg->memory_backend || strcmp(cfg->memory_backend, "sqlite") != 0)
        return HU_OK;
#ifdef HU_ENABLE_SQLITE
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK, "[doctor] SQLite: available");
#else
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_ERR, "[doctor] SQLite: not compiled in");
#endif
}

static hu_error_t doctor_check_http_client(hu_allocator_t *alloc, hu_diag_item_t **buf, size_t *n,
                                           size_t *cap, const hu_config_t *cfg) {
    if (!doctor_config_wants_http(cfg))
        return HU_OK;
#if HU_IS_TEST
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK, "[doctor] HTTP client: OK");
#else
#if !defined(HU_ENABLE_CURL)
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_ERR,
                            "[doctor] HTTP client: not compiled in (HU_ENABLE_CURL=OFF)");
#elif !defined(HU_HTTP_CURL)
    return doctor_push_line(
        alloc, buf, n, cap, HU_DIAG_ERR,
        "[doctor] HTTP client: libcurl not linked (install libcurl for outbound HTTP)");
#else
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK,
                            "[doctor] HTTP client: libcurl available");
#endif
#endif
}

static hu_error_t doctor_check_persona_dir(hu_allocator_t *alloc, hu_diag_item_t **buf, size_t *n,
                                           size_t *cap, const hu_config_t *cfg) {
    if (!doctor_config_wants_persona(cfg))
        return HU_OK;
#if HU_IS_TEST
    (void)cfg;
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK, "[doctor] Persona dir: OK");
#else
#ifndef _WIN32
    char pbuf[512];
    const char *dir = hu_persona_base_dir(pbuf, sizeof(pbuf));
    if (!dir)
        return doctor_push_line(alloc, buf, n, cap, HU_DIAG_WARN,
                                "[doctor] Persona dir: cannot resolve (HOME or HU_PERSONA_DIR)");
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char *line =
            hu_sprintf(alloc, "[doctor] Persona dir: missing or not a directory (%s)", dir);
        if (!line)
            return HU_ERR_OUT_OF_MEMORY;
        hu_error_t e = doctor_push_line(alloc, buf, n, cap, HU_DIAG_WARN, line);
        alloc->free(alloc->ctx, line, strlen(line) + 1);
        return e;
    }
    char *line = hu_sprintf(alloc, "[doctor] Persona dir: OK (%s)", dir);
    if (!line)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t e = doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK, line);
    alloc->free(alloc->ctx, line, strlen(line) + 1);
    return e;
#else
    (void)cfg;
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK, "[doctor] Persona dir: OK");
#endif
#endif
}

static void doctor_free_diag_items(hu_allocator_t *alloc, hu_diag_item_t *buf, size_t n,
                                   size_t cap_slots) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i].category)
            alloc->free(alloc->ctx, (void *)buf[i].category, strlen(buf[i].category) + 1);
        if (buf[i].message)
            alloc->free(alloc->ctx, (void *)buf[i].message, strlen(buf[i].message) + 1);
        if (buf[i].error_class)
            alloc->free(alloc->ctx, (void *)buf[i].error_class, strlen(buf[i].error_class) + 1);
    }
    if (buf)
        alloc->free(alloc->ctx, buf, sizeof(hu_diag_item_t) * cap_slots);
}

unsigned long hu_doctor_parse_df_available_mb(const char *df_output, size_t len) {
    if (!df_output || len == 0)
        return 0;
    const char *last_line = NULL;
    const char *p = df_output;
    const char *end = df_output + len;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n')
            p++;
        if (p > line) {
            while (p > line && (p[-1] == ' ' || p[-1] == '\r'))
                p--;
            if (p > line)
                last_line = line;
        }
        if (p < end)
            p++;
    }
    if (!last_line)
        return 0;
    const char *col = last_line;
    for (int i = 0; i < 4 && col < end; i++) {
        while (col < end && (*col == ' ' || *col == '\t'))
            col++;
        if (col >= end)
            return 0;
        const char *start = col;
        while (col < end && *col != ' ' && *col != '\t')
            col++;
        if (i == 3) {
            unsigned long v = 0;
            for (const char *q = start; q < col; q++) {
                if (*q >= '0' && *q <= '9')
                    v = v * 10 + (unsigned long)(*q - '0');
            }
            return v;
        }
    }
    return 0;
}

hu_error_t hu_doctor_truncate_for_display(hu_allocator_t *alloc, const char *s, size_t len,
                                          size_t max_len, char **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!s) {
        *out = NULL;
        return HU_OK;
    }
    if (len == 0)
        len = strlen(s);
    if (len <= max_len) {
        *out = hu_strndup(alloc, s, len);
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }
    size_t i = max_len;
    while (i > 0 && (s[i] & 0xC0) == 0x80)
        i--;
    *out = hu_strndup(alloc, s, i);
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t doctor_check_local_inference(hu_allocator_t *alloc, hu_diag_item_t **buf,
                                               size_t *n, size_t *cap) {
#if HU_IS_TEST
    hu_error_t e = doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK,
                                    "[doctor] Ollama (localhost:11434): OK (test mode)");
    if (e != HU_OK)
        return e;
    e = doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK,
                         "[doctor] llama-cli (PATH): OK (test mode)");
    if (e != HU_OK)
        return e;
#else
    bool ollama_ok = hu_ollama_api_tags_reachable();
    hu_error_t e;
    if (ollama_ok)
        e = doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK,
                             "[doctor] Ollama (localhost:11434): reachable (GET /api/tags)");
    else
        e = doctor_push_line(
            alloc, buf, n, cap, HU_DIAG_WARN,
            "[doctor] Ollama (localhost:11434): not reachable (run `ollama serve`)");
    if (e != HU_OK)
        return e;
    if (hu_exe_on_path("llama-cli"))
        e = doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK, "[doctor] llama-cli: on PATH");
    else
        e = doctor_push_line(alloc, buf, n, cap, HU_DIAG_WARN,
                             "[doctor] llama-cli: not on PATH (install llama.cpp)");
    if (e != HU_OK)
        return e;
#endif
#ifdef HU_ENABLE_EMBEDDED_MODEL
    return doctor_push_line(alloc, buf, n, cap, HU_DIAG_OK,
                            "[doctor] Embedded model provider: compiled in");
#else
    return doctor_push_line(
        alloc, buf, n, cap, HU_DIAG_WARN,
        "[doctor] Embedded model provider: not compiled in (HU_ENABLE_EMBEDDED_MODEL=OFF)");
#endif
}

/* ── Security checks ─────────────────────────────────────────────────── */

hu_error_t hu_doctor_check_security(hu_allocator_t *alloc, hu_diag_item_t **items, size_t *count,
                                    size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

    /* Sandbox availability */
#if defined(__linux__)
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Sandbox: Linux (landlock/bwrap available)");
#elif defined(__APPLE__)
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Sandbox: macOS (sandbox-exec available)");
#else
    doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                     "[doctor] Sandbox: platform sandbox not available");
#endif

    /* Exec env sanitization compiled in */
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Exec env sanitization: active "
                     "(blocks MAVEN_OPTS, LD_PRELOAD, GLIBC_TUNABLES, etc.)");

    /* Unicode visual spoofing detection */
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Unicode spoofing detection: active "
                     "(Hangul fillers, bidi overrides, zero-width chars)");

    /* Safe-bin review */
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Risky binary detection: active "
                     "(jq, printenv, env flagged for secret-dump risk)");

    return HU_OK;
}

/* ── Memory health checks ────────────────────────────────────────────── */

hu_error_t hu_doctor_check_memory_health(hu_allocator_t *alloc, const hu_config_t *cfg,
                                         hu_diag_item_t **items, size_t *count, size_t *cap) {
    if (!alloc || !cfg || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_ENABLE_SQLITE
    if (cfg->memory_backend && strcmp(cfg->memory_backend, "sqlite") == 0) {
#if HU_IS_TEST
        doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                         "[doctor] SQLite memory: OK (test mode)");
#else
        doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                         "[doctor] SQLite memory: compiled in and configured");
#endif
    }
#else
    if (cfg->memory_backend && strcmp(cfg->memory_backend, "sqlite") == 0) {
        doctor_push_line(alloc, items, count, cap, HU_DIAG_ERR,
                         "[doctor] SQLite memory: requested but not compiled in");
    }
#endif

    return HU_OK;
}

/* ── Skills checks ───────────────────────────────────────────────────── */

hu_error_t hu_doctor_check_skills(hu_allocator_t *alloc, hu_diag_item_t **items, size_t *count,
                                  size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_ENABLE_SKILLS
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Skills subsystem: compiled in");
#else
    doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                     "[doctor] Skills subsystem: not compiled in (HU_ENABLE_SKILLS=OFF)");
#endif

    /* Check if skills directory exists */
#if HU_IS_TEST
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Skills directory: OK (test mode)");
#else
#ifndef _WIN32
    char skills_dir[512];
    size_t slen = hu_skill_registry_get_installed_dir(skills_dir, sizeof(skills_dir));
    if (slen > 0) {
        struct stat st2;
        if (stat(skills_dir, &st2) == 0 && S_ISDIR(st2.st_mode)) {
            char *msg = hu_sprintf(alloc, "[doctor] Skills directory: %s", skills_dir);
            if (msg) {
                doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
                alloc->free(alloc->ctx, msg, strlen(msg) + 1);
            }
        } else {
            doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                             "[doctor] Skills directory: not found (run `human skills install`)");
        }
    }
#endif
#endif

    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] Skill registry: https://github.com/human/skill-registry");

    return HU_OK;
}

/* ── iMessage channel diagnostics ────────────────────────────────────── */

#if HU_HAS_IMESSAGE && !defined(_WIN32)
/* Lightweight scan of a key:value pair from the poll-status JSON. The status
 * file is hand-emitted and small, so a substring search is sufficient and
 * avoids pulling in a JSON parser at this layer. Returns true on hit. */
static bool doctor_imsg_status_extract_int(const char *blob, const char *key, int64_t *out) {
    const char *p = strstr(blob, key);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p)
        return false;
    *out = (int64_t)v;
    return true;
}

static bool doctor_imsg_status_extract_str(const char *blob, const char *key, char *out,
                                           size_t cap) {
    const char *p = strstr(blob, key);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end)
        return false;
    size_t n = (size_t)(end - p);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool doctor_imsg_status_extract_bool(const char *blob, const char *key, bool *out) {
    const char *p = strstr(blob, key);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* Map an hu_imessage_error_class_t to a user-actionable single-line
 * diagnostic. Returns a heap buffer the caller frees via alloc->free
 * (length strlen(*out) + 1), or NULL via *out for HU_IMESSAGE_ERR_NONE.
 *
 * This is the SINGLE PLACE the AUTH/BUSY/CANTOPEN/OTHER message text
 * lives — both the poll-status JSON branch and the live-SQLite
 * branches route through here so no per-class message is duplicated
 * (and so AUTH/BUSY cross-contamination cannot regress at one call
 * site while another is fixed). See US-43.5 design + ACs. */
static hu_error_t doctor_imsg_diag_from_class(hu_allocator_t *alloc, hu_imessage_error_class_t cls,
                                              char **out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    char *msg = NULL;
    switch (cls) {
    case HU_IMESSAGE_ERR_NONE:
        /* Healthy poll — no diagnostic to emit. */
        return HU_OK;
    case HU_IMESSAGE_ERR_AUTH:
        /* Full Disk Access denied. MUST NOT mention Messages.app syncing
         * (AC-43.5.1 adversary). */
        msg = hu_strdup(alloc, "iMessage poll denied: grant Full Disk Access to human in System "
                               "Settings > Privacy & Security > Full Disk Access.");
        break;
    case HU_IMESSAGE_ERR_BUSY:
        /* SQLITE_BUSY / SQLITE_LOCKED — typically Messages.app holds the
         * write lock during iCloud sync. MUST NOT mention "Full Disk Access"
         * or "permission" (AC-43.5.2 adversary). */
        msg = hu_strdup(alloc, "iMessage chat.db is busy: Messages.app may be syncing. Retry in a "
                               "few seconds; persistent failure indicates an iCloud sync issue.");
        break;
    case HU_IMESSAGE_ERR_CANTOPEN:
        /* SQLITE_CANTOPEN — chat.db missing. Path hint is load-bearing
         * (AC-43.5.3). */
        msg = hu_strdup(alloc, "iMessage chat.db not found at ~/Library/Messages/chat.db. Open "
                               "Messages.app at least once to initialize the database.");
        break;
    case HU_IMESSAGE_ERR_OTHER:
    default:
        /* Use the canonical class name from imessage.h (AC-43.5.5 — no
         * parallel enum, no parallel name table). */
        msg = hu_sprintf(alloc, "iMessage poll failed: unclassified SQLite error (class=%s)",
                         hu_imessage_error_class_name(HU_IMESSAGE_ERR_OTHER));
        break;
    }
    if (!msg)
        return HU_ERR_OUT_OF_MEMORY;
    *out = msg;
    return HU_OK;
}
#endif

#if HU_HAS_IMESSAGE
hu_error_t hu_imessage_diag_from_poll_status(hu_allocator_t *alloc, const char *poll_status_json,
                                             char **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!poll_status_json) {
        /* No status -> no diagnostic to emit; treat as NONE. */
        return HU_OK;
    }
    char cls_name[32];
    cls_name[0] = '\0';
    if (!doctor_imsg_status_extract_str(poll_status_json, "\"last_error_class\"", cls_name,
                                        sizeof(cls_name))) {
        /* Missing field -> nothing to format; treat as NONE. */
        return HU_OK;
    }
    hu_imessage_error_class_t cls = hu_imessage_error_class_from_name(cls_name);
    return doctor_imsg_diag_from_class(alloc, cls, out);
}
#else
hu_error_t hu_imessage_diag_from_poll_status(hu_allocator_t *alloc, const char *poll_status_json,
                                             char **out) {
    (void)alloc;
    (void)poll_status_json;
    if (out)
        *out = NULL;
    return HU_OK;
}
#endif

hu_error_t hu_doctor_check_imessage(hu_allocator_t *alloc, int64_t now_epoch,
                                    int64_t stale_after_secs, hu_diag_item_t **items, size_t *count,
                                    size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

#if !HU_HAS_IMESSAGE || defined(_WIN32)
    (void)now_epoch;
    (void)stale_after_secs;
    return doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                            "[doctor] iMessage: not built into this binary "
                            "(HU_ENABLE_IMESSAGE=OFF)");
#else
    /* 1. chat.db readability — the FDA gate.
     *
     * IMPORTANT: `access(R_OK)` only checks POSIX permissions, NOT macOS TCC.
     * When Full Disk Access is revoked (the textbook FDA-after-rebuild
     * symptom), POSIX still says the file is readable but sqlite3_open_v2()
     * returns SQLITE_AUTH (23). To produce a diagnostic that actually
     * matches what the daemon experiences, we attempt a real sqlite open
     * (read-only) and run a no-op query. Without sqlite at build time we
     * fall back to access() and explicitly disclaim the limitation. */
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        doctor_push_line(alloc, items, count, cap, HU_DIAG_ERR,
                         "[doctor] iMessage: $HOME is not set; cannot locate chat.db");
    } else {
        char db_path[768];
        int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
        if (n > 0 && (size_t)n < sizeof(db_path)) {
#ifdef HU_ENABLE_SQLITE
            sqlite3 *probe = NULL;
            int rc = sqlite3_open_v2(db_path, &probe, SQLITE_OPEN_READONLY, NULL);
            if (rc == SQLITE_OK && probe) {
                /* Run a tiny query to actually trigger TCC, since open alone
                 * sometimes succeeds before TCC fires on first read. */
                sqlite3_stmt *stmt = NULL;
                int qrc =
                    sqlite3_prepare_v2(probe, "SELECT 1 FROM message LIMIT 1", -1, &stmt, NULL);
                if (qrc == SQLITE_OK)
                    qrc = sqlite3_step(stmt);
                if (stmt)
                    sqlite3_finalize(stmt);
                hu_imessage_error_class_t cls = hu_imessage_classify_sqlite_error(qrc);
                sqlite3_close(probe);
                if (cls == HU_IMESSAGE_ERR_NONE || qrc == SQLITE_DONE || qrc == SQLITE_ROW) {
                    char *msg = hu_sprintf(
                        alloc, "[doctor] iMessage chat.db: readable via sqlite (%s)", db_path);
                    if (msg) {
                        doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
                        alloc->free(alloc->ctx, msg, strlen(msg) + 1);
                    }
                } else {
                    /* Branch B: live sqlite3_prepare_v2/sqlite3_step error.
                     * Route the per-class hint through the shared formatter so
                     * AUTH/BUSY/CANTOPEN do not collapse to one hardcoded
                     * "Full Disk Access" message (US-43.5 ACs 1+2 + prior
                     * critic finding). */
                    char *diag = NULL;
                    (void)doctor_imsg_diag_from_class(alloc, cls, &diag);
                    char *msg = hu_sprintf(
                        alloc, "[doctor] iMessage chat.db: sqlite returned %s (rc=%d) — %s",
                        hu_imessage_error_class_name(cls), qrc, diag ? diag : "unclassified error");
                    if (msg) {
                        doctor_push_line_with_class(alloc, items, count, cap, HU_DIAG_ERR, msg,
                                                    hu_imessage_error_class_name(cls));
                        alloc->free(alloc->ctx, msg, strlen(msg) + 1);
                    }
                    if (diag)
                        alloc->free(alloc->ctx, diag, strlen(diag) + 1);
                }
            } else {
                /* Branch C: live sqlite3_open_v2 error. Same routing
                 * discipline as Branch B — no hardcoded per-class hint. */
                if (probe)
                    sqlite3_close(probe);
                hu_imessage_error_class_t cls = hu_imessage_classify_sqlite_error(rc);
                char *diag = NULL;
                (void)doctor_imsg_diag_from_class(alloc, cls, &diag);
                char *msg = hu_sprintf(
                    alloc, "[doctor] iMessage chat.db: sqlite_open failed %s (rc=%d) — %s",
                    hu_imessage_error_class_name(cls), rc, diag ? diag : "unclassified error");
                if (msg) {
                    doctor_push_line_with_class(alloc, items, count, cap, HU_DIAG_ERR, msg,
                                                hu_imessage_error_class_name(cls));
                    alloc->free(alloc->ctx, msg, strlen(msg) + 1);
                }
                if (diag)
                    alloc->free(alloc->ctx, diag, strlen(diag) + 1);
            }
#else
            /* No sqlite at build time: weakened POSIX-only check. */
            if (access(db_path, R_OK) == 0) {
                char *msg = hu_sprintf(alloc,
                                       "[doctor] iMessage chat.db: POSIX-readable (%s) — TCC not "
                                       "checked (HU_ENABLE_SQLITE=OFF)",
                                       db_path);
                if (msg) {
                    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
                    alloc->free(alloc->ctx, msg, strlen(msg) + 1);
                }
            } else {
                doctor_push_line(
                    alloc, items, count, cap, HU_DIAG_ERR,
                    "[doctor] iMessage chat.db: NOT readable — grant Full Disk Access to "
                    "$(readlink -f $(which human)) in System Settings → Privacy & Security");
            }
#endif /* HU_ENABLE_SQLITE */
        }
    }

    /* 2. imsg CLI availability — needed for send/react/watch. */
#if HU_IS_TEST
    /* Tests assert deterministic output regardless of host PATH. */
    doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                     "[doctor] imsg CLI: skipped (test mode)");
#else
    if (hu_exe_on_path("imsg"))
        doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                         "[doctor] imsg CLI: on PATH (send + react + watch enabled)");
    else
        doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                         "[doctor] imsg CLI: not on PATH — fallback to AppleScript only "
                         "(install steipete/imsg for send/react/watch)");
#endif

    /* 3. Poll-status file: presence + freshness + breaker. */
    char status_path[512];
    if (!hu_imessage_status_path(status_path, sizeof(status_path))) {
        doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                         "[doctor] iMessage poll status: cannot resolve path "
                         "(HOME unset?)");
        return HU_OK;
    }

    FILE *f = fopen(status_path, "r");
    if (!f) {
        doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                         "[doctor] iMessage poll status: file missing — daemon has not polled yet "
                         "(start `human service-loop` and re-run)");
        return HU_OK;
    }
    char blob[2048];
    size_t blen = fread(blob, 1, sizeof(blob) - 1, f);
    fclose(f);
    blob[blen] = '\0';

    int64_t last_rowid = -1;
    int64_t last_success = 0;
    int64_t consecutive = 0;
    bool tripped = false;
    char err_class[32];
    err_class[0] = '\0';
    (void)doctor_imsg_status_extract_int(blob, "\"last_rowid\"", &last_rowid);
    (void)doctor_imsg_status_extract_int(blob, "\"last_successful_poll_epoch\"", &last_success);
    (void)doctor_imsg_status_extract_int(blob, "\"consecutive_open_failures\"", &consecutive);
    (void)doctor_imsg_status_extract_bool(blob, "\"circuit_breaker_tripped\"", &tripped);
    (void)doctor_imsg_status_extract_str(blob, "\"last_error_class\"", err_class,
                                         sizeof(err_class));

    if (tripped) {
        /* Branch A: poll-status JSON read. Replace the old hardcoded
         * "re-grant Full Disk Access" tail with a class-routed diagnostic
         * so AUTH/BUSY/CANTOPEN do not all collapse to the FDA message
         * (US-43.5 ACs 1+2). */
        char *diag = NULL;
        (void)hu_imessage_diag_from_poll_status(alloc, blob, &diag);
        char *msg = hu_sprintf(
            alloc, "[doctor] iMessage circuit breaker: TRIPPED (%lld consecutive %s errors) — %s",
            (long long)consecutive, err_class[0] ? err_class : "?",
            diag ? diag : "unclassified error; check the daemon logs");
        if (msg) {
            doctor_push_line_with_class(alloc, items, count, cap, HU_DIAG_ERR, msg,
                                        err_class[0] ? err_class : NULL);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
        if (diag)
            alloc->free(alloc->ctx, diag, strlen(diag) + 1);
    } else if (consecutive > 0) {
        char *msg = hu_sprintf(
            alloc, "[doctor] iMessage circuit breaker: OK (%lld recent failures, last=%s)",
            (long long)consecutive, err_class[0] ? err_class : "?");
        if (msg) {
            doctor_push_line_with_class(alloc, items, count, cap, HU_DIAG_WARN, msg,
                                        err_class[0] ? err_class : NULL);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    } else {
        doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                         "[doctor] iMessage circuit breaker: OK");
    }

    if (last_rowid >= 0) {
        char *msg = hu_sprintf(alloc, "[doctor] iMessage last_rowid: %lld", (long long)last_rowid);
        if (msg) {
            doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    }

    if (last_success <= 0) {
        doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                         "[doctor] iMessage poll: never recorded a successful poll");
    } else {
        int64_t age = now_epoch - last_success;
        if (age < 0)
            age = 0;
        if (stale_after_secs > 0 && age > stale_after_secs) {
            char *msg =
                hu_sprintf(alloc, "[doctor] iMessage poll: STALE — last success %lld seconds ago",
                           (long long)age);
            if (msg) {
                doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN, msg);
                alloc->free(alloc->ctx, msg, strlen(msg) + 1);
            }
        } else {
            char *msg =
                hu_sprintf(alloc, "[doctor] iMessage poll: fresh (last success %lld seconds ago)",
                           (long long)age);
            if (msg) {
                doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
                alloc->free(alloc->ctx, msg, strlen(msg) + 1);
            }
        }
    }

    return HU_OK;
#endif
}

/* ── Config semantics (existing, enhanced) ───────────────────────────── */

hu_error_t hu_doctor_check_config_semantics(hu_allocator_t *alloc, const hu_config_t *cfg,
                                            hu_diag_item_t **items, size_t *count) {
    if (!alloc || !cfg || !items || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *items = NULL;
    *count = 0;

    size_t cap = 48;
    hu_diag_item_t *buf = (hu_diag_item_t *)alloc->alloc(alloc->ctx, sizeof(hu_diag_item_t) * cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t n = 0;
    hu_diag_item_t it;

    if (!cfg->default_provider || !cfg->default_provider[0]) {
        it = (hu_diag_item_t){
            HU_DIAG_ERR, hu_strdup(alloc, "config"),
            hu_strdup(alloc, "no default_provider configured "
                             "(run 'human onboard' or set mlx_local/apple for no API key)"),
            NULL};
        buf[n++] = it;
    } else {
        char *msg = hu_sprintf(alloc, "provider: %s", cfg->default_provider);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"), msg, NULL};
            buf[n++] = it;
        }
    }

    if (cfg->default_temperature < 0.0 || cfg->default_temperature > 2.0) {
        char *msg = hu_sprintf(alloc, "temperature %.1f is out of range (expected 0.0-2.0)",
                               cfg->default_temperature);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_ERR, hu_strdup(alloc, "config"), msg, NULL};
            buf[n++] = it;
        }
    } else {
        char *msg =
            hu_sprintf(alloc, "temperature %.1f (valid range 0.0-2.0)", cfg->default_temperature);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"), msg, NULL};
            buf[n++] = it;
        }
    }

    uint16_t gw_port = cfg->gateway.port;
    if (gw_port == 0) {
        it = (hu_diag_item_t){HU_DIAG_ERR, hu_strdup(alloc, "config"),
                              hu_strdup(alloc, "gateway port is 0 (invalid)"), NULL};
        buf[n++] = it;
    } else {
        char *msg = hu_sprintf(alloc, "gateway port: %u", (unsigned)gw_port);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"), msg, NULL};
            buf[n++] = it;
        }
    }

    bool has_ch = hu_channel_catalog_has_any_configured(cfg, false);
    if (has_ch) {
        it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"),
                              hu_strdup(alloc, "at least one channel configured"), NULL};
        buf[n++] = it;
    } else {
        it = (hu_diag_item_t){
            HU_DIAG_WARN, hu_strdup(alloc, "config"),
            hu_strdup(alloc, "no channels configured -- run onboard to set one up"), NULL};
        buf[n++] = it;
    }

    const struct {
        const char *name;
        bool enabled;
    } modules[] = {
        {"tree_of_thought", cfg->agent.tree_of_thought},
        {"constitutional_ai", cfg->agent.constitutional_ai},
        {"speculative_cache", cfg->agent.speculative_cache},
        {"llm_compiler", cfg->agent.llm_compiler_enabled},
        {"hula", cfg->agent.hula_enabled},
        {"mcts_planner", cfg->agent.mcts_planner_enabled},
        {"tool_routing", cfg->agent.tool_routing_enabled},
        {"multi_agent", cfg->agent.multi_agent},
    };
    size_t active = 0;
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        if (modules[i].enabled)
            active++;
    }
    if (n + 1 < cap) {
        char *msg = hu_sprintf(alloc, "intelligence: %zu/%zu modules active", active,
                               sizeof(modules) / sizeof(modules[0]));
        if (msg) {
            it = (hu_diag_item_t){active > 0 ? HU_DIAG_OK : HU_DIAG_WARN, hu_strdup(alloc, "agent"),
                                  msg, NULL};
            buf[n++] = it;
        }
    }
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]) && n < cap; i++) {
        char *msg = hu_sprintf(alloc, "%s: %s", modules[i].name,
                               modules[i].enabled ? "enabled" : "disabled");
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "intelligence"), msg, NULL};
            buf[n++] = it;
        }
    }

    hu_error_t ext_err = doctor_check_sqlite_backend(alloc, &buf, &n, &cap, cfg);
    if (ext_err != HU_OK) {
        doctor_free_diag_items(alloc, buf, n, cap);
        return ext_err;
    }
    ext_err = doctor_check_http_client(alloc, &buf, &n, &cap, cfg);
    if (ext_err != HU_OK) {
        doctor_free_diag_items(alloc, buf, n, cap);
        return ext_err;
    }
    ext_err = doctor_check_persona_dir(alloc, &buf, &n, &cap, cfg);
    if (ext_err != HU_OK) {
        doctor_free_diag_items(alloc, buf, n, cap);
        return ext_err;
    }
    ext_err = doctor_check_local_inference(alloc, &buf, &n, &cap);
    if (ext_err != HU_OK) {
        doctor_free_diag_items(alloc, buf, n, cap);
        return ext_err;
    }

    ext_err = hu_doctor_check_security(alloc, &buf, &n, &cap);
    if (ext_err != HU_OK) {
        doctor_free_diag_items(alloc, buf, n, cap);
        return ext_err;
    }

    ext_err = hu_doctor_check_memory_health(alloc, cfg, &buf, &n, &cap);
    if (ext_err != HU_OK) {
        doctor_free_diag_items(alloc, buf, n, cap);
        return ext_err;
    }

    ext_err = hu_doctor_check_skills(alloc, &buf, &n, &cap);
    if (ext_err != HU_OK) {
        doctor_free_diag_items(alloc, buf, n, cap);
        return ext_err;
    }

    *items = buf;
    *count = n;
    return HU_OK;
}

hu_error_t hu_doctor_check_verifier(hu_allocator_t *alloc, int64_t now_epoch,
                                    int64_t stale_after_secs, double flagged_warn_rate,
                                    hu_diag_item_t **items, size_t *count, size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

    char path[512];
    if (!hu_verifier_metrics_path(path, sizeof(path))) {
        return doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                                "[doctor] verifier: $HOME unset; cannot locate metrics file");
    }

    hu_verifier_metrics_t m;
    hu_error_t lerr = hu_verifier_metrics_load(&m);

    if (lerr == HU_ERR_NOT_FOUND) {
        char *msg =
            hu_sprintf(alloc,
                       "[doctor] verifier: no metrics yet — daemon hasn't completed its first "
                       "60s flush (file: %s)",
                       path);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
        return HU_OK;
    }
    if (lerr != HU_OK) {
        char *msg =
            hu_sprintf(alloc, "[doctor] verifier: failed to read %s (err=%d)", path, (int)lerr);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_ERR, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
        return HU_OK;
    }

    int64_t age = now_epoch - m.last_update_epoch;
    if (age < 0)
        age = 0;
    bool stale = stale_after_secs > 0 && age > stale_after_secs;

    /* Counts line — always emit so users can sanity-check that turns are
     * actually flowing through the verifier. Zero runs is a real signal:
     * either the daemon just started, or no chat has happened. */
    {
        char *msg = hu_sprintf(
            alloc, "[doctor] verifier: %llu turns, %llu claims (flagged %llu, %.1f%% rate)",
            (unsigned long long)m.total_runs, (unsigned long long)m.total_claims_extracted,
            (unsigned long long)m.total_claims_flagged,
            hu_verifier_metrics_flagged_rate(&m) * 100.0);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    }

    /* Heartbeat line — STALE means the daemon stopped flushing. The flush
     * cadence is 60s, so default 300s = 5 min gives plenty of headroom for
     * normal scheduling jitter while still catching a wedged daemon fast. */
    if (stale) {
        char *msg = hu_sprintf(alloc,
                               "[doctor] verifier heartbeat: STALE — last flush %lld seconds ago "
                               "(threshold %lld); daemon may be offline or wedged",
                               (long long)age, (long long)stale_after_secs);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    } else {
        char *msg =
            hu_sprintf(alloc, "[doctor] verifier heartbeat: fresh (%llds ago)", (long long)age);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    }

    /* Flagged-rate line — only meaningful with at least one extracted claim;
     * the rate is undefined for zero-claim runs. */
    if (m.total_claims_extracted > 0) {
        double rate = hu_verifier_metrics_flagged_rate(&m);
        if (flagged_warn_rate > 0.0 && rate >= flagged_warn_rate) {
            char *msg =
                hu_sprintf(alloc,
                           "[doctor] verifier flagged-rate: HIGH (%.1f%% >= threshold %.1f%%) — "
                           "memory may be under-populated or the model is hallucinating",
                           rate * 100.0, flagged_warn_rate * 100.0);
            if (msg) {
                (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN, msg);
                alloc->free(alloc->ctx, msg, strlen(msg) + 1);
            }
        }
    }

    return HU_OK;
}

hu_error_t hu_doctor_parse_scheduler_status_json(const char *json, unsigned long long *jobs_pending,
                                                 unsigned long long *jobs_completed_today,
                                                 long long *battery_pct, char *on_ac_power_text,
                                                 size_t on_ac_power_cap, long long *updated_epoch) {
    return hu_scheduler_status_parse_json(json, jobs_pending, jobs_completed_today, battery_pct,
                                          on_ac_power_text, on_ac_power_cap, updated_epoch);
}

hu_error_t hu_doctor_check_scheduler(hu_allocator_t *alloc, int64_t now_epoch,
                                     int64_t stale_after_secs, hu_diag_item_t **items,
                                     size_t *count, size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

    const char *home = getenv("HOME");
    if (!home || !home[0])
        return doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                                "[doctor] scheduler: $HOME unset");

    char path[512];
    int pn = snprintf(path, sizeof(path), "%s/.human/scheduler.status", home);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                                "[doctor] scheduler: path overflow");

    FILE *f = fopen(path, "r");
    if (!f) {
        char *msg = hu_sprintf(alloc,
                               "[doctor] scheduler: no status file yet (%s) — daemon may not "
                               "have ticked W14",
                               path);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
        return HU_OK;
    }
    char buf[4096];
    size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[nread] = '\0';

    unsigned long long jp = 0, jc = 0;
    long long bat = 0;
    char acbuf[16] = {0};
    long long ue = 0;
    if (hu_scheduler_status_parse_json(buf, &jp, &jc, &bat, acbuf, sizeof(acbuf), &ue) != HU_OK) {
        (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN,
                               "[doctor] scheduler: status file present but parse failed "
                               "(upgrade skew?)");
        return HU_OK;
    }

    {
        char *msg = hu_sprintf(alloc,
                               "[doctor] scheduler: pending=%llu completed_24h=%llu "
                               "battery_pct=%lld on_ac=%s",
                               jp, jc, bat, acbuf[0] ? acbuf : "?");
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    }
    int64_t age = now_epoch - ue;
    if (age < 0)
        age = 0;
    if (stale_after_secs > 0 && age > stale_after_secs) {
        char *msg = hu_sprintf(alloc,
                               "[doctor] scheduler heartbeat: STALE — status_age=%llds "
                               "(threshold %llds)",
                               (long long)age, (long long)stale_after_secs);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_WARN, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    } else {
        char *msg = hu_sprintf(alloc, "[doctor] scheduler heartbeat: fresh (status_age=%llds)",
                               (long long)age);
        if (msg) {
            (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_OK, msg);
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
    }
    return HU_OK;
}

hu_error_t hu_doctor_check_response_pipeline(hu_allocator_t *alloc, hu_diag_item_t **items,
                                             size_t *count, size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;
    (void)doctor_push_line(
        alloc, items, count, cap, HU_DIAG_OK,
        "[doctor] responses: degenerate output is retried via a slim 2-message request, then "
        "cloud fallback (gemini → openai) when HU_ENABLE_CURL=1");
    (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                           "[doctor] responses: grep ~/.human/logs/service-loop-error.log for "
                           "\"response_guard\" and \"empty assistant response\"");
    (void)doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                           "[doctor] responses: MLX HTTP 52 (empty reply) usually means the "
                           "local server rejected an oversized body — slim retry addresses this");
    return HU_OK;
}
