/* src/agent/init_outcome.c
 *
 * Append-only JSONL outcome log for the init_proposer subsystem.
 * See include/human/agent/init_outcome.h.
 *
 * Per .claude/rules/silent-config-gated-subsystems.md: when the file is
 * unwritable, emit ONE log line per process — don't silently lose
 * proposals just because the disk is full or the parent dir is missing. */

#include "human/agent/init_outcome.h"
#include "human/core/log.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define HU_INIT_OUTCOME_FILENAME "initiative_proposals.jsonl"

#if HU_IS_TEST
static const char *s_path_override = NULL;
void hu_init_outcome_set_path_for_test(const char *path) {
    s_path_override = path;
}
#else
void hu_init_outcome_set_path_for_test(const char *path) {
    (void)path;
}
#endif

static atomic_bool s_warned_io = false;

/* Map verdict enum → stable string (matches the JSON schema doc). */
static const char *verdict_to_string(hu_init_proposer_result_t v) {
    switch (v) {
    case HU_INIT_RESULT_FIRED:
        return "FIRED";
    case HU_INIT_RESULT_LOW_CONFIDENCE:
        return "LOW_CONFIDENCE";
    case HU_INIT_RESULT_NEGATIVE:
        return "NEGATIVE";
    case HU_INIT_RESULT_LLM_ERROR:
        return "LLM_ERROR";
    case HU_INIT_RESULT_PARSE_ERROR:
        return "PARSE_ERROR";
    case HU_INIT_RESULT_SKIP:
        return "SKIP";
    case HU_INIT_RESULT_GATED_QUIET:
        return "GATED_QUIET";
    case HU_INIT_RESULT_GATED_BUDGET:
        return "GATED_BUDGET";
    case HU_INIT_RESULT_GATED_RECENCY:
        return "GATED_RECENCY";
    case HU_INIT_RESULT_GATED_INTERVAL:
        return "GATED_INTERVAL";
    default:
        return "UNKNOWN";
    }
}

/* Write a JSON string into buf[pos..] with escaping for ", \, control chars.
 * Returns new pos. Truncates rather than overflows. Always NUL-terminates. */
static size_t json_escape(char *buf, size_t cap, size_t pos, const char *s, size_t s_len) {
    if (!buf || cap == 0)
        return pos;
    if (pos >= cap - 1)
        return cap - 1;
    if (!s)
        return pos;
    for (size_t i = 0; i < s_len && pos + 2 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (pos + 2 >= cap - 1)
                break;
            buf[pos++] = '\\';
            buf[pos++] = (char)c;
        } else if (c == '\n') {
            if (pos + 2 >= cap - 1)
                break;
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == '\r') {
            if (pos + 2 >= cap - 1)
                break;
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (c == '\t') {
            if (pos + 2 >= cap - 1)
                break;
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if (c < 0x20) {
            /* Skip other control chars — JSON parsers reject them and
             * they're unlikely to be meaningful in a proposal draft. */
            continue;
        } else {
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    return pos;
}

size_t hu_init_outcome_resolve_path(char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap == 0)
        return 0;
    out_buf[0] = '\0';
#if HU_IS_TEST
    if (s_path_override) {
        size_t l = strlen(s_path_override);
        size_t copy = l < out_cap - 1 ? l : out_cap - 1;
        memcpy(out_buf, s_path_override, copy);
        out_buf[copy] = '\0';
        return copy;
    }
#endif
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return 0;
    int n = snprintf(out_buf, out_cap, "%s/.human/%s", home, HU_INIT_OUTCOME_FILENAME);
    if (n <= 0)
        return 0;
    return (size_t)n < out_cap ? (size_t)n : out_cap - 1;
}

/* Ensure the parent directory exists (creates ~/.human if needed). */
static void ensure_parent_dir(const char *path) {
    if (!path || !path[0])
        return;
    /* find last slash */
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path)
        return;
    size_t dir_len = (size_t)(last_slash - path);
    char dir[1024];
    if (dir_len >= sizeof(dir))
        return;
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';
    /* mkdir is no-op if exists; rwx for user only (matches the rest
     * of ~/.human's permissions). */
    (void)mkdir(dir, 0700);
}

hu_error_t hu_init_outcome_append(hu_allocator_t *alloc, int64_t ts_unix, uint64_t tick_id,
                                  hu_init_proposer_result_t verdict,
                                  const hu_init_decision_t *decision, const char *target_handle,
                                  bool dry_run) {
    (void)alloc; /* not used yet — file I/O via stdio */
    if (!decision)
        return HU_ERR_INVALID_ARGUMENT;

    char path[1024];
    if (hu_init_outcome_resolve_path(path, sizeof(path)) == 0) {
        hu_log_warn_once(&s_warned_io, "init_outcome", NULL,
                         "could not resolve $HOME/.human/initiative_proposals.jsonl path "
                         "(HOME unset?) — proposals not persisted");
        return HU_ERR_IO;
    }

    ensure_parent_dir(path);

    /* Build the JSON line in a stack buffer — bounded by HU_INIT_DRAFT_MAX
     * + HU_INIT_SKIP_REASON_MAX + ~256 bytes of schema fields. 2 KB is
     * plenty of headroom. */
    char line[2048];
    size_t pos = 0;
    int n = snprintf(
        line, sizeof(line),
        "{\"schema\":\"init_outcome_v1\",\"ts_unix\":%lld,\"tick_id\":%llu,\"verdict\":\"%s\","
        "\"confidence\":%.3f,\"dry_run\":%s,\"draft\":\"",
        (long long)ts_unix, (unsigned long long)tick_id, verdict_to_string(verdict),
        decision->confidence, dry_run ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(line))
        return HU_ERR_IO;
    pos = (size_t)n;

    pos = json_escape(line, sizeof(line), pos, decision->draft, decision->draft_len);

    /* Append ", reason field, escape reason, close JSON, newline. */
    if (pos + 12 >= sizeof(line))
        return HU_ERR_IO;
    memcpy(line + pos, "\",\"reason\":\"", 12);
    pos += 12;
    pos = json_escape(line, sizeof(line), pos, decision->skip_reason, decision->skip_reason_len);

    if (pos + 12 >= sizeof(line))
        return HU_ERR_IO;
    memcpy(line + pos, "\",\"target\":\"", 12);
    pos += 12;
    if (target_handle) {
        pos = json_escape(line, sizeof(line), pos, target_handle, strlen(target_handle));
    }

    if (pos + 3 >= sizeof(line))
        return HU_ERR_IO;
    memcpy(line + pos, "\"}\n", 3);
    pos += 3;
    line[pos] = '\0';

    FILE *f = fopen(path, "a");
    if (!f) {
        hu_log_warn_once(&s_warned_io, "init_outcome", NULL,
                         "could not open %s for append (errno=%d) — proposals not persisted", path,
                         errno);
        return HU_ERR_IO;
    }
    size_t wrote = fwrite(line, 1, pos, f);
    int close_err = fclose(f);
    if (wrote != pos || close_err != 0) {
        hu_log_warn_once(&s_warned_io, "init_outcome", NULL,
                         "short write or close failure on %s (wrote=%zu expected=%zu close=%d)",
                         path, wrote, pos, close_err);
        return HU_ERR_IO;
    }
    return HU_OK;
}
