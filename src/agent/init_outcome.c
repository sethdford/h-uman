/* src/agent/init_outcome.c
 *
 * Append-only JSONL outcome log for the init_proposer subsystem.
 * See include/human/agent/init_outcome.h.
 *
 * Per .claude/rules/silent-config-gated-subsystems.md: when the file is
 * unwritable, emit ONE log line per process — don't silently lose
 * proposals just because the disk is full or the parent dir is missing. */

#include "human/agent/init_outcome.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

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

/* ──────────────────────────────────────────────────────────────────────────
 * CLI viewer — `human initiative log|status`.
 *
 * Read-only over the JSONL written by hu_init_outcome_append above.
 * Both subcommands tolerate the file being missing or empty (prints a
 * helpful message rather than erroring). */

void hu_init_outcome_aggregate_line(hu_init_status_t *status, const char *line, size_t line_len) {
    if (!status || !line || line_len == 0)
        return;
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    if (hu_json_parse(&alloc, line, line_len, &root) != HU_OK || !root) {
        if (root)
            hu_json_free(&alloc, root);
        return; /* malformed line — count nothing, don't crash */
    }
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(&alloc, root);
        return;
    }

    const char *verdict = hu_json_get_string(root, "verdict");
    if (!verdict) {
        hu_json_free(&alloc, root);
        return;
    }
    status->total++;
    status->sum_confidence += hu_json_get_number(root, "confidence", 0.0);

    if (strcmp(verdict, "FIRED") == 0) {
        status->count_fired++;
        int64_t ts = (int64_t)hu_json_get_number(root, "ts_unix", 0.0);
        if (ts > status->last_fired_ts_unix)
            status->last_fired_ts_unix = ts;
    } else if (strcmp(verdict, "LOW_CONFIDENCE") == 0) {
        status->count_low_confidence++;
    } else if (strcmp(verdict, "NEGATIVE") == 0) {
        status->count_negative++;
    } else if (strcmp(verdict, "LLM_ERROR") == 0) {
        status->count_llm_error++;
    } else if (strcmp(verdict, "PARSE_ERROR") == 0) {
        status->count_parse_error++;
    } else {
        status->count_unknown_verdict++;
    }
    hu_json_free(&alloc, root);
}

/* Read entire JSONL file into a heap-allocated buffer. Returns NULL on
 * any failure (file missing, too big, read error). Caller frees via
 * alloc->free. Caps at 10 MB so a runaway file doesn't OOM the CLI. */
#define HU_INIT_OUTCOME_MAX_FILE_BYTES (10u * 1024u * 1024u)

static char *read_jsonl_file(hu_allocator_t *alloc, const char *path, size_t *out_len) {
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || (unsigned long)sz > HU_INIT_OUTCOME_MAX_FILE_BYTES) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Format a unix timestamp as "YYYY-MM-DD HH:MM:SS" in local time. */
static void format_local_time(int64_t ts_unix, char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return;
    time_t t = (time_t)ts_unix;
    struct tm lt;
    if (!localtime_r(&t, &lt)) {
        snprintf(out, out_cap, "<bad-ts>");
        return;
    }
    strftime(out, out_cap, "%Y-%m-%d %H:%M:%S", &lt);
}

/* Pretty-print one JSONL line to stdout. Tolerates malformed lines
 * silently (no output if parse fails). */
static void print_decision_line(const char *line, size_t line_len) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    if (hu_json_parse(&alloc, line, line_len, &root) != HU_OK || !root) {
        if (root)
            hu_json_free(&alloc, root);
        return;
    }
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(&alloc, root);
        return;
    }
    int64_t ts = (int64_t)hu_json_get_number(root, "ts_unix", 0.0);
    const char *verdict = hu_json_get_string(root, "verdict");
    double conf = hu_json_get_number(root, "confidence", 0.0);
    bool dry_run = hu_json_get_bool(root, "dry_run", false);
    const char *target = hu_json_get_string(root, "target");
    const char *draft = hu_json_get_string(root, "draft");
    const char *reason = hu_json_get_string(root, "reason");

    char ts_str[32];
    format_local_time(ts, ts_str, sizeof(ts_str));
    /* Header line: timestamp + verdict + confidence + dry-run flag + target. */
    printf("%s  %-14s  conf=%.2f%s%s%s\n", ts_str, verdict ? verdict : "?", conf,
           dry_run ? "  (dry-run)" : "  (LIVE)   ", (target && target[0]) ? "  → " : "",
           (target && target[0]) ? target : "");
    /* Detail line: draft or reason, whichever is non-empty. */
    if (draft && draft[0])
        printf("                     draft : %s\n", draft);
    if (reason && reason[0])
        printf("                     reason: %s\n", reason);
    printf("\n");
    hu_json_free(&alloc, root);
}

/* Walk the buffer line-by-line; for each line, invoke the visitor.
 * Lines are NOT NUL-terminated when passed to visitor — visitor must
 * respect line_len. */
typedef void (*hu_init_outcome_line_visitor_t)(void *ctx, const char *line, size_t line_len);

static void for_each_line(const char *buf, size_t buf_len, hu_init_outcome_line_visitor_t visitor,
                          void *ctx) {
    size_t start = 0;
    for (size_t i = 0; i <= buf_len; i++) {
        if (i == buf_len || buf[i] == '\n') {
            if (i > start)
                visitor(ctx, buf + start, i - start);
            start = i + 1;
        }
    }
}

/* Visitor that accumulates the last N line offsets so we can render
 * only the tail. Cap at HU_INIT_OUTCOME_LOG_MAX_LAST entries; 1000 is
 * generous and bounded. */
#define HU_INIT_OUTCOME_LOG_MAX_LAST 1000
typedef struct line_ring {
    const char *lines[HU_INIT_OUTCOME_LOG_MAX_LAST];
    size_t lens[HU_INIT_OUTCOME_LOG_MAX_LAST];
    size_t count;
    size_t cap;
} line_ring_t;

static void ring_visitor(void *ctx, const char *line, size_t line_len) {
    line_ring_t *r = (line_ring_t *)ctx;
    if (r->count < r->cap) {
        r->lines[r->count] = line;
        r->lens[r->count] = line_len;
        r->count++;
    } else {
        /* shift left (cheap: rare cap-overflow path; max cap=1000) */
        memmove(r->lines, r->lines + 1, (r->cap - 1) * sizeof(r->lines[0]));
        memmove(r->lens, r->lens + 1, (r->cap - 1) * sizeof(r->lens[0]));
        r->lines[r->cap - 1] = line;
        r->lens[r->cap - 1] = line_len;
    }
}

static void status_visitor(void *ctx, const char *line, size_t line_len) {
    hu_init_outcome_aggregate_line((hu_init_status_t *)ctx, line, line_len);
}

static hu_error_t cmd_initiative_log(hu_allocator_t *alloc, int argc, char **argv) {
    size_t last_n = 10;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
            long n = strtol(argv[i + 1], NULL, 10);
            if (n > 0) {
                last_n = (size_t)n;
                if (last_n > HU_INIT_OUTCOME_LOG_MAX_LAST)
                    last_n = HU_INIT_OUTCOME_LOG_MAX_LAST;
            }
            i++;
        }
    }
    char path[1024];
    if (hu_init_outcome_resolve_path(path, sizeof(path)) == 0) {
        fprintf(stderr, "init_outcome: $HOME not set; cannot resolve log path\n");
        return HU_ERR_IO;
    }
    size_t buf_len = 0;
    char *buf = read_jsonl_file(alloc, path, &buf_len);
    if (!buf) {
        printf("No proposals yet at %s.\n", path);
        printf("The init_proposer ticks every 30 min while initiative.enabled=true.\n");
        printf("First non-gated tick writes the first record.\n");
        return HU_OK;
    }
    line_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    ring.cap = last_n;
    for_each_line(buf, buf_len, ring_visitor, &ring);
    printf("Last %zu of %zu proposals at %s:\n\n", ring.count, ring.count, path);
    for (size_t i = 0; i < ring.count; i++) {
        print_decision_line(ring.lines[i], ring.lens[i]);
    }
    alloc->free(alloc->ctx, buf, buf_len + 1);
    return HU_OK;
}

static hu_error_t cmd_initiative_status(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    char path[1024];
    if (hu_init_outcome_resolve_path(path, sizeof(path)) == 0) {
        fprintf(stderr, "init_outcome: $HOME not set; cannot resolve log path\n");
        return HU_ERR_IO;
    }
    size_t buf_len = 0;
    char *buf = read_jsonl_file(alloc, path, &buf_len);
    if (!buf) {
        printf("No proposals yet at %s.\n", path);
        return HU_OK;
    }
    hu_init_status_t status;
    memset(&status, 0, sizeof(status));
    for_each_line(buf, buf_len, status_visitor, &status);

    double mean_conf = status.total > 0 ? status.sum_confidence / (double)status.total : 0.0;

    printf("Initiative proposer status\n");
    printf("──────────────────────────\n");
    printf("File:           %s\n", path);
    printf("Total decisions: %zu\n", status.total);
    printf("\n");
    printf("By verdict:\n");
    printf("  FIRED            %5zu  (%.1f%%)\n", status.count_fired,
           status.total ? 100.0 * (double)status.count_fired / (double)status.total : 0.0);
    printf("  LOW_CONFIDENCE   %5zu  (%.1f%%)\n", status.count_low_confidence,
           status.total ? 100.0 * (double)status.count_low_confidence / (double)status.total : 0.0);
    printf("  NEGATIVE         %5zu  (%.1f%%)\n", status.count_negative,
           status.total ? 100.0 * (double)status.count_negative / (double)status.total : 0.0);
    printf("  LLM_ERROR        %5zu  (%.1f%%)\n", status.count_llm_error,
           status.total ? 100.0 * (double)status.count_llm_error / (double)status.total : 0.0);
    printf("  PARSE_ERROR      %5zu  (%.1f%%)\n", status.count_parse_error,
           status.total ? 100.0 * (double)status.count_parse_error / (double)status.total : 0.0);
    if (status.count_unknown_verdict > 0)
        printf("  UNKNOWN          %5zu  (schema drift?)\n", status.count_unknown_verdict);
    printf("\n");
    printf("Mean confidence (across all verdicts): %.3f\n", mean_conf);
    if (status.last_fired_ts_unix > 0) {
        char ts_str[32];
        format_local_time(status.last_fired_ts_unix, ts_str, sizeof(ts_str));
        printf("Last FIRED:                            %s\n", ts_str);
    } else {
        printf("Last FIRED:                            (none yet)\n");
    }
    alloc->free(alloc->ctx, buf, buf_len + 1);
    return HU_OK;
}

hu_error_t cmd_initiative(hu_allocator_t *alloc, int argc, char **argv) {
    /* h-uman dispatch passes the FULL argv (argv[0]=program path,
     * argv[1]="initiative", argv[2]=sub-subcommand). Matches the
     * convention used by cmd_autoresponder. */
    if (argc < 3) {
        fprintf(stderr,
                "Usage: human initiative <log [--last N] | status>\n"
                "\n"
                "  log     Pretty-print the last N (default 10) non-gated decisions.\n"
                "  status  Aggregate counts by verdict + mean confidence + last fire time.\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *sub = argv[2];
    if (strcmp(sub, "log") == 0)
        return cmd_initiative_log(alloc, argc - 3, argv + 3);
    if (strcmp(sub, "status") == 0)
        return cmd_initiative_status(alloc, argc - 3, argv + 3);
    fprintf(stderr, "human initiative: unknown subcommand '%s' (try 'log' or 'status')\n", sub);
    return HU_ERR_INVALID_ARGUMENT;
}
