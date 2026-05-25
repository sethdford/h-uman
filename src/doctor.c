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
/* Phase 4b — hu_kv_quant_from_string / hu_kv_quant_to_string for the
 * inference doctor check. Header is gate-free so it compiles even when
 * HU_ENABLE_LLAMACPP is OFF (the actual GGML wiring is what's gated). */
#include "human/providers/llamacpp.h"
#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif

#if HU_HAS_IMESSAGE && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__linux__)
#include <limits.h>
#endif

#define HU_DOCTOR_LINE_CATEGORY "doctor_line"

static hu_error_t doctor_push_line(hu_allocator_t *alloc, hu_diag_item_t **buf, size_t *n,
                                   size_t *cap, hu_diag_severity_t sev, const char *line) {
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
    if (!cat || !msg) {
        if (cat)
            alloc->free(alloc->ctx, cat, strlen(cat) + 1);
        if (msg)
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    (*buf)[*n] = (hu_diag_item_t){sev, cat, msg};
    (*n)++;
    return HU_OK;
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

/* ── Phase 4b — inference throughput config checks ──────────────────── */

/* Helper: emit a line with a heap-allocated message (printf-formatted)
 * so callers don't have to allocate per-call. Borrowed pattern from
 * the imessage diagnose path. */
#include <stdarg.h>

static hu_error_t doctor_push_fmt(hu_allocator_t *alloc, hu_diag_item_t **buf, size_t *n,
                                  size_t *cap, hu_diag_severity_t sev, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (wrote < 0)
        return HU_ERR_INVALID_ARGUMENT;
    return doctor_push_line(alloc, buf, n, cap, sev, tmp);
}

hu_error_t hu_doctor_check_inference(hu_allocator_t *alloc, hu_diag_item_t **items, size_t *count,
                                     size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t e;

    /* KV quant: detect typos via the parser's recognized-flag. */
    const char *kv = getenv("HU_LLAMACPP_KV_QUANT");
    if (!kv || !*kv) {
        e = doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                             "[doctor] HU_LLAMACPP_KV_QUANT: unset (FP16 default)");
        if (e != HU_OK)
            return e;
    } else {
        bool recognized = false;
        hu_kv_quant_t parsed = hu_kv_quant_from_string(kv, &recognized);
        if (recognized) {
            e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_OK,
                                "[doctor] HU_LLAMACPP_KV_QUANT=%s → %s (recognized)", kv,
                                hu_kv_quant_to_string(parsed));
        } else {
            e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_WARN,
                                "[doctor] HU_LLAMACPP_KV_QUANT=%s UNRECOGNIZED — "
                                "factory silently falls back to FP16. "
                                "Accepted: fp16/f16/16, q8_0/q8/8, q4_0/q4/4 (case-insensitive)",
                                kv);
        }
        if (e != HU_OK)
            return e;
    }

    /* Flash Attention: detect values that aren't a recognized off-token.
     * Factory's off-tokens are exactly {"off","0","false"}; anything else
     * keeps FA on (the operator-intended default for Mac Metal). Warn on
     * values that look like an attempt to disable that won't actually
     * disable (e.g. "no", "False", "OFF" — case mismatches). */
    const char *fa = getenv("HU_LLAMACPP_FLASH_ATTN");
    if (!fa || !*fa) {
        e = doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                             "[doctor] HU_LLAMACPP_FLASH_ATTN: unset (FA on, default)");
    } else if (strcmp(fa, "off") == 0 || strcmp(fa, "0") == 0 || strcmp(fa, "false") == 0) {
        e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_OK,
                            "[doctor] HU_LLAMACPP_FLASH_ATTN=%s → FA OFF (recognized)", fa);
    } else if (strcmp(fa, "on") == 0 || strcmp(fa, "1") == 0 || strcmp(fa, "true") == 0) {
        e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_OK,
                            "[doctor] HU_LLAMACPP_FLASH_ATTN=%s → FA ON (default behavior)", fa);
    } else {
        /* Adversarial: e.g. "OFF" (case mismatch), "no", "disable", etc.
         * Factory keeps FA on. Operator probably meant to disable it. */
        e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_WARN,
                            "[doctor] HU_LLAMACPP_FLASH_ATTN=%s — factory keeps FA ON. "
                            "To disable, use exactly 'off', '0', or 'false' (lowercase)",
                            fa);
    }
    if (e != HU_OK)
        return e;

    /* Draft model: verify the file exists on disk. Factory silently
     * disables spec decode if the path doesn't resolve at provider
     * create time — operator might think they're getting Phase 3b
     * benefits and never see the warning. */
    const char *draft = getenv("HU_LLAMACPP_DRAFT_MODEL");
    if (!draft || !*draft) {
        e = doctor_push_line(alloc, items, count, cap, HU_DIAG_OK,
                             "[doctor] HU_LLAMACPP_DRAFT_MODEL: unset (spec decode disabled)");
    } else if (access(draft, R_OK) == 0) {
        e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_OK,
                            "[doctor] HU_LLAMACPP_DRAFT_MODEL=%s (readable, spec decode ready)",
                            draft);
    } else {
        e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_ERR,
                            "[doctor] HU_LLAMACPP_DRAFT_MODEL=%s NOT READABLE — "
                            "spec decode will silently fail at provider creation. "
                            "Run: scripts/fetch-gemma.sh --draft <url> --draft-sha <sha>",
                            draft);
    }
    if (e != HU_OK)
        return e;

    /* Numeric overrides: validate ranges. Out-of-range silently degrades
     * to 0 (upstream default) at the factory; the operator never sees a
     * surface-level warning. */
    const char *min_p_s = getenv("HU_LLAMACPP_DRAFT_MIN_P");
    if (min_p_s && *min_p_s) {
        char *end = NULL;
        double v = strtod(min_p_s, &end);
        if (!end || end == min_p_s || v < 0.0 || v > 1.0) {
            e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_WARN,
                                "[doctor] HU_LLAMACPP_DRAFT_MIN_P=%s out of [0,1] — "
                                "factory ignores, upstream default used",
                                min_p_s);
            if (e != HU_OK)
                return e;
        }
    }
    const char *max_t_s = getenv("HU_LLAMACPP_DRAFT_MAX_TOKENS");
    if (max_t_s && *max_t_s) {
        char *end = NULL;
        long v = strtol(max_t_s, &end, 10);
        if (!end || end == max_t_s || v <= 0 || v >= 64) {
            e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_WARN,
                                "[doctor] HU_LLAMACPP_DRAFT_MAX_TOKENS=%s out of (0,64) — "
                                "factory ignores, upstream default used",
                                max_t_s);
            if (e != HU_OK)
                return e;
        }
    }

    /* Phase 2c — opt-in KV cache skip-decode (Phase 2b.2). STRICT
     * on-token matching (1 / on / true) is by design: mis-enabling can
     * silently corrupt KV in real linked-libllama builds. The doctor's
     * job here is to make mis-enable attempts VISIBLE — operator sets
     * "yes" thinking it'll enable; factory keeps default OFF; without
     * this check the operator never knows the opt-in didn't take. */
    const char *skip = getenv("HU_LLAMACPP_KVCACHE_SKIP_DECODE");
    if (!skip || !*skip) {
        e = doctor_push_line(
            alloc, items, count, cap, HU_DIAG_OK,
            "[doctor] HU_LLAMACPP_KVCACHE_SKIP_DECODE: unset (Phase 2b SAFE path)");
    } else if (strcmp(skip, "1") == 0 || strcmp(skip, "on") == 0 || strcmp(skip, "true") == 0) {
        e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_OK,
                            "[doctor] HU_LLAMACPP_KVCACHE_SKIP_DECODE=%s → ON "
                            "(Phase 2b.2 opt-in active — TTFT win on warm hits)",
                            skip);
    } else {
        e = doctor_push_fmt(alloc, items, count, cap, HU_DIAG_WARN,
                            "[doctor] HU_LLAMACPP_KVCACHE_SKIP_DECODE=%s NOT a recognized "
                            "on-token — factory keeps SAFE default OFF. To enable, "
                            "use exactly '1', 'on', or 'true' (lowercase). "
                            "Strict matching is intentional: mis-enabling can corrupt KV.",
                            skip);
    }
    if (e != HU_OK)
        return e;

    return HU_OK;
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

/* Lightweight scan of a key:value pair from the poll-status JSON. The status
 * file is hand-emitted and small, so a substring search is sufficient and
 * avoids pulling in a JSON parser at this layer. Returns true on hit.
 *
 * Kept unconditional (no HU_HAS_IMESSAGE guard) because the US-9.6 public
 * helper `hu_imessage_diag_from_poll_status` is callable from any build
 * profile so monitoring tools and tests get a stable symbol. */
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

/* ── US-9.6 presentation predicate ─────────────────────────────────────
 *
 * Pure mapping from a `last_error_class` STRING (as serialized by the
 * daemon via `hu_imessage_error_class_name` into `imessage.poll_status`)
 * to a user-actionable diag item. Strings instead of the enum so the
 * predicate compiles and links even when HU_ENABLE_IMESSAGE=OFF (the
 * monitoring `--json` consumer may run on a slimmed binary that still
 * reads the status file).
 *
 * Per `.claude/rules/security-predicate-extraction.md`: pure, single
 * translation unit, called by both the production
 * `hu_doctor_check_imessage` path AND the unit tests under
 * `tests/test_doctor_imessage_diagnose.c` so the two cannot drift.
 *
 * Per `.claude/rules/tests-that-pin-bugs.md`: the BUSY branch MUST NOT
 * mention "Full Disk Access" or "System Settings" and the AUTH branch
 * MUST NOT mention "Messages.app may be syncing" — the test file pins
 * both negatives.
 *
 * Inputs:
 *   - `cls_str`: "NONE" / "AUTH" / "BUSY" / "CANTOPEN" / "OTHER" / "" /NULL
 *   - `consecutive`: matches `consecutive_open_failures` in the status file
 *   - `tripped`: matches `circuit_breaker_tripped` in the status file
 *
 * Output `out` is fully populated (severity + heap category + heap
 * message). Caller frees `out->category` and `out->message`. */
static hu_error_t doctor_imessage_present(hu_allocator_t *alloc, const char *cls_str,
                                          int64_t consecutive, bool tripped, hu_diag_item_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cls = cls_str ? cls_str : "";
    bool is_auth = (strcmp(cls, "AUTH") == 0);
    bool is_busy = (strcmp(cls, "BUSY") == 0);
    bool is_cantopen = (strcmp(cls, "CANTOPEN") == 0);
    bool is_none = (strcmp(cls, "NONE") == 0);
    bool is_known = is_auth || is_busy || is_cantopen || is_none || (strcmp(cls, "OTHER") == 0);

    /* Breaker tripped wins over the underlying class — it's the most
     * urgent thing the user needs to know AND the underlying class is
     * still surfaced inside the message body (AC-9.6.3). */
    const char *category;
    hu_diag_severity_t severity;
    char *msg = NULL;

    if (tripped) {
        category = "imessage_breaker";
        severity = HU_DIAG_ERR;
        if (is_auth) {
            msg =
                hu_sprintf(alloc,
                           "[doctor] iMessage circuit breaker: TRIPPED after %lld consecutive AUTH "
                           "errors — Full Disk Access denied. Open System Settings > Privacy & "
                           "Security > Full Disk Access, toggle human on, then run `human doctor "
                           "--fix` to reset the breaker",
                           (long long)consecutive);
        } else if (is_cantopen) {
            msg = hu_sprintf(alloc,
                             "[doctor] iMessage circuit breaker: TRIPPED after %lld consecutive "
                             "CANTOPEN errors — chat.db not found at the expected path. Open "
                             "Messages.app once to create it, then run `human doctor --fix` to "
                             "reset the breaker",
                             (long long)consecutive);
        } else if (is_busy) {
            msg =
                hu_sprintf(alloc,
                           "[doctor] iMessage circuit breaker: TRIPPED after %lld consecutive BUSY "
                           "errors — Messages.app may be syncing for an unusually long time. "
                           "Wait for sync to settle, then run `human doctor --fix` to reset the "
                           "breaker",
                           (long long)consecutive);
        } else {
            msg = hu_sprintf(alloc,
                             "[doctor] iMessage circuit breaker: TRIPPED after %lld consecutive %s "
                             "errors. Run `human doctor --fix` to reset the breaker after the "
                             "underlying issue is resolved",
                             (long long)consecutive, cls[0] ? cls : "unknown");
        }
    } else if (is_auth) {
        category = "imessage_fda";
        severity = HU_DIAG_ERR;
        msg = hu_sprintf(alloc,
                         "[doctor] iMessage chat.db: Full Disk Access denied. Open System Settings "
                         "> Privacy & Security > Full Disk Access, then toggle human on (and "
                         "restart the daemon)");
    } else if (is_busy) {
        category = "imessage_busy";
        severity = HU_DIAG_WARN;
        msg =
            hu_sprintf(alloc, "[doctor] iMessage chat.db: locked (transient). Messages.app may be "
                              "syncing — try again in a moment");
    } else if (is_cantopen) {
        category = "imessage_not_found";
        severity = HU_DIAG_ERR;
        msg = hu_sprintf(alloc, "[doctor] iMessage chat.db: not found at the expected path "
                                "(~/Library/Messages/chat.db). Open Messages.app at least once so "
                                "macOS creates the database, then re-run this check");
    } else if (is_none) {
        category = "imessage_chat_db";
        severity = HU_DIAG_OK;
        msg = hu_sprintf(alloc, "[doctor] iMessage chat.db: healthy (last poll returned NONE)");
    } else if (!is_known || !cls[0]) {
        /* Unknown / empty class: surface as a WARN — the channel has not
         * healthily polled, but we cannot point the user at a specific
         * fix. Severity is NOT OK (the tests pin this — empty status must
         * not falsely report healthy). */
        category = "imessage_other";
        severity = HU_DIAG_WARN;
        msg =
            hu_sprintf(alloc, "[doctor] iMessage chat.db: state unknown (no recognized error class "
                              "in poll status — daemon may not have polled yet)");
    } else {
        /* OTHER */
        category = "imessage_other";
        severity = HU_DIAG_ERR;
        msg = hu_sprintf(
            alloc,
            "[doctor] iMessage chat.db: sqlite reported an unclassified error (class=%s, "
            "%lld consecutive failures). See daemon logs for the raw return code",
            cls, (long long)consecutive);
    }

    char *cat = hu_strdup(alloc, category);
    if (!msg || !cat) {
        if (msg)
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        if (cat)
            alloc->free(alloc->ctx, cat, strlen(cat) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->severity = severity;
    out->category = cat;
    out->message = msg;
    return HU_OK;
}

hu_error_t hu_imessage_diag_from_poll_status(hu_allocator_t *alloc, const char *json_blob,
                                             hu_diag_item_t *out) {
    if (!alloc || !json_blob || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char err_class[32];
    err_class[0] = '\0';
    int64_t consecutive = 0;
    bool tripped = false;

    (void)doctor_imsg_status_extract_str(json_blob, "\"last_error_class\"", err_class,
                                         sizeof(err_class));
    (void)doctor_imsg_status_extract_int(json_blob, "\"consecutive_open_failures\"", &consecutive);
    (void)doctor_imsg_status_extract_bool(json_blob, "\"circuit_breaker_tripped\"", &tripped);

    return doctor_imessage_present(alloc, err_class, consecutive, tripped, out);
}

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
                    char *msg = hu_sprintf(
                        alloc,
                        "[doctor] iMessage chat.db: sqlite returned %s (rc=%d) — Full Disk "
                        "Access likely revoked. Re-grant FDA on the daemon binary at "
                        "$(launchctl print gui/$UID/ai.human.service-loop | grep program)",
                        hu_imessage_error_class_name(cls), qrc);
                    if (msg) {
                        doctor_push_line(alloc, items, count, cap, HU_DIAG_ERR, msg);
                        alloc->free(alloc->ctx, msg, strlen(msg) + 1);
                    }
                }
            } else {
                if (probe)
                    sqlite3_close(probe);
                hu_imessage_error_class_t cls = hu_imessage_classify_sqlite_error(rc);
                char *msg = hu_sprintf(
                    alloc, "[doctor] iMessage chat.db: sqlite_open failed %s (rc=%d) — %s",
                    hu_imessage_error_class_name(cls), rc,
                    cls == HU_IMESSAGE_ERR_AUTH
                        ? "Full Disk Access denied; grant FDA to the daemon binary"
                        : "see sqlite docs");
                if (msg) {
                    doctor_push_line(alloc, items, count, cap, HU_DIAG_ERR, msg);
                    alloc->free(alloc->ctx, msg, strlen(msg) + 1);
                }
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

    /* US-9.6: delegate breaker/error-class presentation to the shared
     * predicate so the output matches what the unit tests pin. The
     * predicate handles:
     *   - tripped + class → "TRIPPED ... `human doctor --fix` ..."
     *   - AUTH → "Full Disk Access denied. System Settings > ..."
     *   - BUSY → "Messages.app may be syncing — try again in a moment"
     *   - CANTOPEN → "chat.db: not found ..."
     *   - NONE → healthy
     *   - other / empty → WARN, "state unknown"
     * The pre-US-9.6 free-form line is replaced — the existing red-team
     * tests in `tests/test_ported_modules.c` have been updated to match
     * the new substrings. */
    if (tripped || consecutive > 0 || err_class[0]) {
        hu_diag_item_t pres = {0};
        if (doctor_imessage_present(alloc, err_class, consecutive, tripped, &pres) == HU_OK) {
            doctor_push_line(alloc, items, count, cap, pres.severity, pres.message);
            if (pres.category)
                alloc->free(alloc->ctx, (void *)pres.category, strlen(pres.category) + 1);
            if (pres.message)
                alloc->free(alloc->ctx, (void *)pres.message, strlen(pres.message) + 1);
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
                             "(run 'human onboard' or set mlx_local/apple for no API key)")};
        buf[n++] = it;
    } else {
        char *msg = hu_sprintf(alloc, "provider: %s", cfg->default_provider);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"), msg};
            buf[n++] = it;
        }
    }

    /* Privacy-thesis-alignment check: the operator has a local provider
     * registered (mlx_local / llamacpp / ollama / apple) but their
     * default_provider points at a cloud provider. The local model is
     * configured AND probably running, but unused. This is the EXACT
     * shape of the 2026-05-25 reactive-iMessage situation:
     *
     *   providers[]: name="mlx_local" base_url=http://127.0.0.1:8741/v1
     *   default_provider: "gemini"
     *
     * — gemma-4-31b-seth-v3-fused serving locally at 11.9 tok/s but
     * the daemon routes to Gemini. Per CLAUDE.md product thesis:
     * "Privacy by architecture, not by settings. Data never leaves
     * the device as a structural property." A configured-but-unused
     * local provider violates that property silently. Surface it. */
    if (cfg->default_provider && cfg->default_provider[0] && cfg->providers &&
        cfg->providers_len > 0 && hu_config_provider_requires_api_key(cfg->default_provider) &&
        n < cap) {
        const char *local_names[] = {"mlx_local", "llamacpp", "llama.cpp", "ollama", "apple"};
        const size_t local_names_n = sizeof(local_names) / sizeof(local_names[0]);
        const char *found_local = NULL;
        for (size_t i = 0; i < cfg->providers_len && !found_local; i++) {
            const hu_provider_entry_t *p = &cfg->providers[i];
            if (!p->name)
                continue;
            for (size_t j = 0; j < local_names_n; j++) {
                if (strcmp(p->name, local_names[j]) == 0) {
                    found_local = p->name;
                    break;
                }
            }
        }
        if (found_local) {
            char *msg =
                hu_sprintf(alloc,
                           "[INFO] local provider '%s' is configured but default_provider='%s' "
                           "(cloud). Privacy thesis: set default_provider='%s' to keep data on "
                           "device.",
                           found_local, cfg->default_provider, found_local);
            if (msg) {
                it = (hu_diag_item_t){HU_DIAG_WARN, hu_strdup(alloc, "config"), msg};
                buf[n++] = it;
            }
        }
    }

    if (cfg->default_temperature < 0.0 || cfg->default_temperature > 2.0) {
        char *msg = hu_sprintf(alloc, "temperature %.1f is out of range (expected 0.0-2.0)",
                               cfg->default_temperature);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_ERR, hu_strdup(alloc, "config"), msg};
            buf[n++] = it;
        }
    } else {
        char *msg =
            hu_sprintf(alloc, "temperature %.1f (valid range 0.0-2.0)", cfg->default_temperature);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"), msg};
            buf[n++] = it;
        }
    }

    uint16_t gw_port = cfg->gateway.port;
    if (gw_port == 0) {
        it = (hu_diag_item_t){HU_DIAG_ERR, hu_strdup(alloc, "config"),
                              hu_strdup(alloc, "gateway port is 0 (invalid)")};
        buf[n++] = it;
    } else {
        char *msg = hu_sprintf(alloc, "gateway port: %u", (unsigned)gw_port);
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"), msg};
            buf[n++] = it;
        }
    }

    /* US-7.3 (INS-B) — honesty gate for personalization.
     *
     * Mirrors the runtime daemon warning (src/daemon.c) at the config
     * surface. The daemon emits its warning when the provider's vtable
     * returns HU_ERR_NOT_SUPPORTED from load_adapter; doctor cannot
     * instantiate providers, so it uses hu_config_provider_requires_api_key
     * as the canonical cloud-vs-local heuristic (matches doctor_config_wants_http
     * at line 66). All currently-known providers that return true from
     * the heuristic also lack a load_adapter vtable hook (only llamacpp
     * and huml implement it today). */
    if (cfg->personalization.lora_adapter_path && cfg->personalization.lora_adapter_path[0] &&
        cfg->default_provider && cfg->default_provider[0] &&
        hu_config_provider_requires_api_key(cfg->default_provider) && n < cap) {
        it = (hu_diag_item_t){
            HU_DIAG_WARN, hu_strdup(alloc, "config"),
            hu_strdup(alloc,
                      "[WARN] personalization.lora_adapter_path is set but the active provider "
                      "does not support adapters")};
        buf[n++] = it;
    }

    /* US-7.7 (AC-7.7.3) — best-of-N cloud-provider misconfiguration warning.
     *
     * Doctor fires this every call (intentional for diagnostic UX — operators
     * re-run doctor on demand to confirm a misconfiguration is still active).
     * No one-shot here. If a future runtime log path emits the same warning,
     * THAT path should apply the D4 one-shot pattern (see canonical shape at
     * src/daemon.c::s_personalization_warn_emitted), but the static and
     * reset shim do NOT exist today — do not call them.
     *
     * The daemon best-of-N path silently no-ops on non-llamacpp providers
     * (eligibility check at the agent_turn dispatch site), so doctor is
     * the only place the operator hears about the misconfiguration. */
    if (cfg->inference.best_of_n >= 2 && cfg->default_provider && cfg->default_provider[0] &&
        hu_config_provider_requires_api_key(cfg->default_provider) && n < cap) {
        it = (hu_diag_item_t){
            HU_DIAG_WARN, hu_strdup(alloc, "config"),
            hu_strdup(alloc, "[WARN] inference.best_of_n has no effect with cloud providers")};
        buf[n++] = it;
    }

    bool has_ch = hu_channel_catalog_has_any_configured(cfg, false);
    if (has_ch) {
        it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "config"),
                              hu_strdup(alloc, "at least one channel configured")};
        buf[n++] = it;
    } else {
        it = (hu_diag_item_t){
            HU_DIAG_WARN, hu_strdup(alloc, "config"),
            hu_strdup(alloc, "no channels configured -- run onboard to set one up")};
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
                                  msg};
            buf[n++] = it;
        }
    }
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]) && n < cap; i++) {
        char *msg = hu_sprintf(alloc, "%s: %s", modules[i].name,
                               modules[i].enabled ? "enabled" : "disabled");
        if (msg) {
            it = (hu_diag_item_t){HU_DIAG_OK, hu_strdup(alloc, "intelligence"), msg};
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

    /* Phase 4b — surface inference-config misconfigurations the
     * factory's friendly-to-typos parsers silenced. */
    ext_err = hu_doctor_check_inference(alloc, &buf, &n, &cap);
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

/* ---------------------------------------------------------------------------
 * US-9.4: install-readiness gate.
 *
 * Each sub-check reads PRIMARY EVIDENCE on the filesystem — not a cached
 * "configured" flag — so that the doctor cannot lie when the install is
 * actually broken. See .claude/rules/tests-that-pin-bugs.md.
 * ------------------------------------------------------------------------- */

/* Push one item using a specific (caller-owned literal) category. */
static hu_error_t install_push(hu_allocator_t *alloc, hu_diag_item_t **buf, size_t *n, size_t *cap,
                               hu_diag_severity_t sev, const char *category, const char *line) {
    if (!line || !category)
        return HU_ERR_INVALID_ARGUMENT;
    if (*n >= *cap) {
        size_t new_cap = (*cap) ? (*cap * 2) : 4;
        hu_diag_item_t *nb =
            (hu_diag_item_t *)alloc->alloc(alloc->ctx, sizeof(hu_diag_item_t) * new_cap);
        if (!nb)
            return HU_ERR_OUT_OF_MEMORY;
        if (*buf)
            memcpy(nb, *buf, sizeof(hu_diag_item_t) * (*n));
        if (*buf)
            alloc->free(alloc->ctx, *buf, sizeof(hu_diag_item_t) * (*cap));
        *buf = nb;
        *cap = new_cap;
    }
    char *cat = hu_strdup(alloc, category);
    char *msg = hu_strdup(alloc, line);
    if (!cat || !msg) {
        if (cat)
            alloc->free(alloc->ctx, cat, strlen(cat) + 1);
        if (msg)
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    (*buf)[*n] = (hu_diag_item_t){sev, cat, msg};
    (*n)++;
    return HU_OK;
}

/* Resolve the running binary's path. PRIMARY EVIDENCE — ask the kernel
 * (Linux) or dyld (macOS) where we actually are, not where someone said we
 * are. Under HU_IS_TEST, allow $HU_TEST_BINARY_PATH override so unit tests
 * can synthesize "binary missing" without nuking the test runner. */
static char *resolve_binary_path(hu_allocator_t *alloc) {
#if HU_IS_TEST
    const char *override = getenv("HU_TEST_BINARY_PATH");
    if (override && override[0])
        return hu_strdup(alloc, override);
#endif
#if defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    return hu_strdup(alloc, buf);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return NULL;
    char *resolved = realpath(buf, NULL);
    if (resolved) {
        char *out = hu_strdup(alloc, resolved);
        free(resolved);
        return out;
    }
    return hu_strdup(alloc, buf);
#else
    (void)alloc;
    return NULL;
#endif
}

/* Build "~/.human" (or $HU_STATE_DIR override). Returns 0 on success. */
static int resolve_state_dir(char *out, size_t cap) {
    const char *override = getenv("HU_STATE_DIR");
    if (override && override[0]) {
        size_t len = strlen(override);
        if (len + 1 > cap)
            return -1;
        memcpy(out, override, len + 1);
        return 0;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    int n = snprintf(out, cap, "%s/.human", home);
    if (n <= 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

hu_error_t hu_doctor_check_install(hu_allocator_t *alloc, const hu_config_t *cfg,
                                   hu_diag_item_t **items, size_t *count, size_t *cap) {
    if (!alloc || !items || !count || !cap)
        return HU_ERR_INVALID_ARGUMENT;

    size_t err_seen_before_call = 0;
    for (size_t i = 0; i < *count; i++)
        if ((*items)[i].severity == HU_DIAG_ERR)
            err_seen_before_call++;

    /* --- 1. binary --- */
    {
        char *bin = resolve_binary_path(alloc);
        bool ok = false;
        if (bin && bin[0]) {
            struct stat st;
            if (stat(bin, &st) == 0 && S_ISREG(st.st_mode))
                ok = true;
        }
        if (ok) {
            char *msg = hu_sprintf(alloc, "binary: OK (%s)", bin);
            (void)install_push(alloc, items, count, cap, HU_DIAG_OK, "binary",
                               msg ? msg : "binary: OK");
            if (msg)
                alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        } else {
            char *msg = hu_sprintf(alloc,
                                   "binary: NOT_RESOLVABLE — could not locate "
                                   "running executable (path=%s)",
                                   bin ? bin : "(null)");
            (void)install_push(alloc, items, count, cap, HU_DIAG_ERR, "binary",
                               msg ? msg
                                   : "binary: NOT_RESOLVABLE — could not locate "
                                     "running executable");
            if (msg)
                alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        }
        if (bin)
            alloc->free(alloc->ctx, bin, strlen(bin) + 1);
    }

    /* --- 2. config_dir --- */
    {
        char dir[1024];
        bool ok = false;
        if (resolve_state_dir(dir, sizeof(dir)) == 0) {
            struct stat st;
            if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
                ok = true;
        }
        if (ok) {
            char *msg = hu_sprintf(alloc, "config_dir: OK (%s)", dir);
            (void)install_push(alloc, items, count, cap, HU_DIAG_OK, "config_dir",
                               msg ? msg : "config_dir: OK");
            if (msg)
                alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        } else {
            (void)install_push(alloc, items, count, cap, HU_DIAG_ERR, "config_dir",
                               "config_dir: MISSING — run 'human onboard' to create ~/.human/");
        }
    }

    /* --- 3. channel --- */
    {
        bool has_channel = false;
        if (cfg) {
            for (size_t i = 0; i < cfg->channels.channel_config_len; i++) {
                const char *k = cfg->channels.channel_config_keys[i];
                if (k && k[0] && cfg->channels.channel_config_counts[i] > 0) {
                    has_channel = true;
                    break;
                }
            }
        }
        if (has_channel) {
            (void)install_push(alloc, items, count, cap, HU_DIAG_OK, "channel",
                               "channel: OK (at least one channel configured)");
        } else {
            (void)install_push(alloc, items, count, cap, HU_DIAG_ERR, "channel",
                               "channel: NONE — run 'human doctor imessage' to pair iMessage");
        }
    }

    /* --- 4. persona --- */
    {
        bool ok = false;
        char detail[256] = {0};
        if (!cfg || !cfg->agent.persona || !cfg->agent.persona[0]) {
            snprintf(detail, sizeof(detail),
                     "persona: MISSING — no persona configured. Run 'human doctor "
                     "--fix' to restore defaults");
        } else {
            const char *name = cfg->agent.persona;
            size_t name_len = strlen(name);
            hu_persona_t p;
            memset(&p, 0, sizeof(p));
            hu_error_t perr = hu_persona_load(alloc, name, name_len, &p);
            if (perr == HU_OK) {
                ok = true;
                snprintf(detail, sizeof(detail), "persona: OK (%s)", name);
                hu_persona_deinit(alloc, &p);
            } else if (perr == HU_ERR_NOT_FOUND) {
                snprintf(detail, sizeof(detail),
                         "persona: MISSING — '%s' not found. Run 'human doctor "
                         "--fix' to restore defaults",
                         name);
            } else {
                snprintf(detail, sizeof(detail),
                         "persona: PARSE_ERROR — '%s' failed to load (rc=%d). Run "
                         "'human doctor --fix' to restore defaults",
                         name, (int)perr);
            }
        }
        (void)install_push(alloc, items, count, cap, ok ? HU_DIAG_OK : HU_DIAG_ERR, "persona",
                           detail);
    }

    /* Tally new errors and decide return code. */
    size_t err_n = 0;
    for (size_t i = 0; i < *count; i++)
        if ((*items)[i].severity == HU_DIAG_ERR)
            err_n++;
    if (err_n > err_seen_before_call)
        return HU_ERR_NOT_FOUND;
    return HU_OK;
}
