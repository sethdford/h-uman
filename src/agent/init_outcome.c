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
#ifdef HU_ENABLE_ML
#include "human/ml/init_dpo_bridge.h"
#endif
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
    case HU_INIT_RESULT_GUARD_REJECT:
        return "GUARD_REJECT";
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

    /* Resolution lines (schema=init_outcome_resolution_v1) bump the
     * resolution counters but do NOT contribute to `total`/proposal
     * counts. Schema-dispatch first so the verdict-string check below
     * doesn't accidentally count resolution lines (they have no
     * "verdict" field, so they'd no-op, but explicit is clearer). */
    const char *schema = hu_json_get_string(root, "schema");
    if (schema && strcmp(schema, "init_outcome_resolution_v1") == 0) {
        const char *outcome = hu_json_get_string(root, "outcome");
        if (outcome && strcmp(outcome, "replied") == 0)
            status->count_resolution_replied++;
        else if (outcome && strcmp(outcome, "ignored") == 0)
            status->count_resolution_ignored++;
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
    /* Reply-detection telemetry (T8 slice 3). Pending = FIRED - replied -
     * ignored, computed inline so operators see the queue depth without
     * a separate field. */
    size_t resolved = status.count_resolution_replied + status.count_resolution_ignored;
    size_t pending = (status.count_fired > resolved) ? (status.count_fired - resolved) : 0;
    printf("\n");
    printf("Reply outcomes (FIRED only):\n");
    printf("  replied          %5zu\n", status.count_resolution_replied);
    printf("  ignored          %5zu\n", status.count_resolution_ignored);
    printf("  pending (in window OR no chat.db) %5zu\n", pending);
    alloc->free(alloc->ctx, buf, buf_len + 1);
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Reply detection (T8 slice 3).
 *
 * Pure decision + sqlite query. The pure predicate is fully tested; the
 * chat.db read is compiled out under HU_IS_TEST so the resolver walk
 * remains testable without a real Messages DB. */

#define HU_MAC_EPOCH_OFFSET_SECS 978307200LL /* unix ts of 2001-01-01 UTC */

hu_init_resolution_t hu_init_outcome_decide_resolution(int64_t proposal_ts, int64_t now_unix,
                                                       bool has_reply, int64_t reply_ts,
                                                       int min_observation_secs,
                                                       int max_window_secs) {
    int obs =
        min_observation_secs > 0 ? min_observation_secs : HU_INIT_OUTCOME_MIN_OBSERVATION_SECS;
    int win = max_window_secs > 0 ? max_window_secs : HU_INIT_OUTCOME_REPLY_WINDOW_SECS;

    /* Always require the observation window before deciding anything —
     * Seth might just be slow to look at the message. */
    if (now_unix - proposal_ts < obs)
        return HU_INIT_RESOLUTION_PENDING;

    /* A reply that arrived BEFORE the proposal is irrelevant noise. */
    if (has_reply && reply_ts > proposal_ts && reply_ts <= proposal_ts + win)
        return HU_INIT_RESOLUTION_REPLIED;

    /* No valid reply within window → mark ignored once window elapses. */
    if (now_unix - proposal_ts >= win)
        return HU_INIT_RESOLUTION_IGNORED;

    return HU_INIT_RESOLUTION_PENDING;
}

hu_error_t hu_init_outcome_append_resolution(hu_allocator_t *alloc, int64_t now_unix,
                                             int64_t proposal_ts, const char *target,
                                             hu_init_resolution_t outcome, int64_t reply_at) {
    (void)alloc;
    char path[1024];
    if (hu_init_outcome_resolve_path(path, sizeof(path)) == 0)
        return HU_ERR_IO;
    ensure_parent_dir(path);

    const char *outcome_str = (outcome == HU_INIT_RESOLUTION_REPLIED) ? "replied" : "ignored";
    char line[1024];
    int n =
        snprintf(line, sizeof(line),
                 "{\"schema\":\"init_outcome_resolution_v1\",\"ts_unix\":%lld,\"proposal_ts\":%lld,"
                 "\"outcome\":\"%s\",\"reply_at\":%lld,\"target\":\"",
                 (long long)now_unix, (long long)proposal_ts, outcome_str, (long long)reply_at);
    if (n <= 0 || (size_t)n >= sizeof(line))
        return HU_ERR_IO;
    size_t pos = (size_t)n;
    if (target)
        pos = json_escape(line, sizeof(line), pos, target, strlen(target));
    if (pos + 3 >= sizeof(line))
        return HU_ERR_IO;
    memcpy(line + pos, "\"}\n", 3);
    pos += 3;

    FILE *f = fopen(path, "a");
    if (!f)
        return HU_ERR_IO;
    size_t wrote = fwrite(line, 1, pos, f);
    int close_err = fclose(f);
    if (wrote != pos || close_err != 0)
        return HU_ERR_IO;
    return HU_OK;
}

/* In-memory pairing: walk JSONL, identify FIRED proposals AND resolution
 * lines, return a list of unresolved (proposal_ts, target, dry_run). */
typedef struct pending_proposal {
    int64_t proposal_ts;
    char target[64];
    bool dry_run;
    /* Captured at parse time so the resolver can hand it to the DPO
     * bridge without re-reading the JSONL. Bounded at 1024 — drafts are
     * typically <500 chars; longer is truncated. */
    char draft[1024];
} pending_proposal_t;

#define HU_INIT_RESOLVE_MAX_PENDING 256

typedef struct pairing_state {
    pending_proposal_t pending[HU_INIT_RESOLVE_MAX_PENDING];
    size_t pending_count;
    int64_t resolved_ts[HU_INIT_RESOLVE_MAX_PENDING * 2];
    size_t resolved_count;
} pairing_state_t;

static bool is_already_resolved(const pairing_state_t *st, int64_t ts) {
    for (size_t i = 0; i < st->resolved_count; i++)
        if (st->resolved_ts[i] == ts)
            return true;
    return false;
}

static void pair_visitor(void *ctx, const char *line, size_t line_len) {
    pairing_state_t *st = (pairing_state_t *)ctx;
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
    const char *schema = hu_json_get_string(root, "schema");
    if (!schema) {
        hu_json_free(&alloc, root);
        return;
    }
    if (strcmp(schema, "init_outcome_resolution_v1") == 0) {
        int64_t pt = (int64_t)hu_json_get_number(root, "proposal_ts", 0.0);
        if (pt > 0 && st->resolved_count < sizeof(st->resolved_ts) / sizeof(st->resolved_ts[0])) {
            st->resolved_ts[st->resolved_count++] = pt;
        }
    } else if (strcmp(schema, "init_outcome_v1") == 0) {
        const char *verdict = hu_json_get_string(root, "verdict");
        bool dry_run = hu_json_get_bool(root, "dry_run", false);
        int64_t ts = (int64_t)hu_json_get_number(root, "ts_unix", 0.0);
        const char *target = hu_json_get_string(root, "target");
        if (verdict && strcmp(verdict, "FIRED") == 0 && ts > 0 && target && target[0] &&
            st->pending_count < HU_INIT_RESOLVE_MAX_PENDING) {
            pending_proposal_t *p = &st->pending[st->pending_count++];
            p->proposal_ts = ts;
            p->dry_run = dry_run;
            size_t tlen = strlen(target);
            size_t copy = tlen < sizeof(p->target) - 1 ? tlen : sizeof(p->target) - 1;
            memcpy(p->target, target, copy);
            p->target[copy] = '\0';
            /* Capture the draft for the DPO bridge. Optional in the
             * JSONL schema (LOW_CONFIDENCE records have it too); only
             * present on FIRED rows here by virtue of the verdict
             * filter above. */
            const char *draft = hu_json_get_string(root, "draft");
            if (draft) {
                size_t dlen = strlen(draft);
                size_t dcopy = dlen < sizeof(p->draft) - 1 ? dlen : sizeof(p->draft) - 1;
                memcpy(p->draft, draft, dcopy);
                p->draft[dcopy] = '\0';
            } else {
                p->draft[0] = '\0';
            }
        }
    }
    hu_json_free(&alloc, root);
}

/* Query chat.db for the first inbound message from `target` after
 * `since_unix`. Returns HU_OK and sets has_reply/reply_at on success
 * (whether or not a row was found). HU_IS_TEST builds skip the query
 * and return has_reply=false. */
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>

static hu_error_t query_chat_db_for_reply(const char *target, int64_t since_unix, bool *has_reply,
                                          int64_t *reply_at) {
    *has_reply = false;
    *reply_at = 0;
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_IO;
    char db_path[1024];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n <= 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_IO;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }
    /* Apple ns = (unix_secs - HU_MAC_EPOCH_OFFSET_SECS) * 1e9. */
    int64_t since_apple_ns = (since_unix - HU_MAC_EPOCH_OFFSET_SECS) * 1000000000LL;
    const char *sql = "SELECT m.date FROM message m JOIN handle h ON m.handle_id = h.rowid "
                      "WHERE h.id = ? AND m.is_from_me = 0 AND m.date > ? "
                      "ORDER BY m.date ASC LIMIT 1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_text(st, 1, target, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, since_apple_ns);
    if (sqlite3_step(st) == SQLITE_ROW) {
        int64_t reply_apple_ns = sqlite3_column_int64(st, 0);
        *reply_at = (reply_apple_ns / 1000000000LL) + HU_MAC_EPOCH_OFFSET_SECS;
        *has_reply = true;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return HU_OK;
}
#else
static hu_error_t query_chat_db_for_reply(const char *target, int64_t since_unix, bool *has_reply,
                                          int64_t *reply_at) {
    (void)target;
    (void)since_unix;
    *has_reply = false;
    *reply_at = 0;
    return HU_OK;
}
#endif

hu_error_t hu_init_outcome_resolve_pending(hu_allocator_t *alloc, int64_t now_unix,
                                           size_t *out_resolutions_written) {
    if (out_resolutions_written)
        *out_resolutions_written = 0;
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    char path[1024];
    if (hu_init_outcome_resolve_path(path, sizeof(path)) == 0)
        return HU_OK; /* no path, no work */

    size_t buf_len = 0;
    char *buf = read_jsonl_file(alloc, path, &buf_len);
    if (!buf)
        return HU_OK; /* no file, no proposals to resolve */

    pairing_state_t st;
    memset(&st, 0, sizeof(st));
    for_each_line(buf, buf_len, pair_visitor, &st);
    alloc->free(alloc->ctx, buf, buf_len + 1);

    size_t written = 0;
    for (size_t i = 0; i < st.pending_count; i++) {
        const pending_proposal_t *p = &st.pending[i];
        if (p->dry_run)
            continue; /* nothing was actually sent */
        if (is_already_resolved(&st, p->proposal_ts))
            continue; /* prior tick wrote a resolution */
        /* Cheap gate: still inside observation window? */
        if (now_unix - p->proposal_ts < HU_INIT_OUTCOME_MIN_OBSERVATION_SECS)
            continue;

        bool has_reply = false;
        int64_t reply_at = 0;
        (void)query_chat_db_for_reply(p->target, p->proposal_ts, &has_reply, &reply_at);

        hu_init_resolution_t verdict = hu_init_outcome_decide_resolution(
            p->proposal_ts, now_unix, has_reply, reply_at, HU_INIT_OUTCOME_MIN_OBSERVATION_SECS,
            HU_INIT_OUTCOME_REPLY_WINDOW_SECS);

        if (verdict == HU_INIT_RESOLUTION_PENDING)
            continue;

        if (hu_init_outcome_append_resolution(alloc, now_unix, p->proposal_ts, p->target, verdict,
                                              reply_at) == HU_OK) {
            written++;
            /* DPO bridge — close the M2 learning loop. Resolution line
             * is the authoritative record (already appended above); the
             * dpo row is derived signal. Bridge failure does NOT cause
             * us to retry or skip subsequent proposals — per design
             * D9, losing a single derived signal is acceptable. */
#ifdef HU_ENABLE_ML
            {
                static atomic_bool s_warned_bridge_unavailable = false;
                static atomic_bool s_warned_bridge_failure = false;
                hu_error_t bridge_err =
                    hu_init_dpo_bridge_record(alloc, verdict, p->draft, p->target, now_unix);
                if (bridge_err == HU_ERR_NOT_SUPPORTED) {
                    hu_log_info_once(&s_warned_bridge_unavailable, "init_outcome", NULL,
                                     "init_outcome → dpo bridge inactive (no dpo_collector "
                                     "registered); resolution lines still appended to JSONL");
                } else if (bridge_err != HU_OK) {
                    hu_log_warn_once(&s_warned_bridge_failure, "init_outcome", NULL,
                                     "init_outcome → dpo bridge record failed (signal lost; "
                                     "JSONL is still authoritative)");
                }
            }
#else
            {
                static atomic_bool s_warned_ml_disabled = false;
                hu_log_info_once(&s_warned_ml_disabled, "init_outcome", NULL,
                                 "init_outcome → dpo bridge unavailable (HU_ENABLE_ML not "
                                 "compiled in); JSONL still records the outcome");
            }
#endif
        }
    }
    if (out_resolutions_written)
        *out_resolutions_written = written;
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
