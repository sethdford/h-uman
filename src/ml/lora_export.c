/* src/ml/lora_export.c
 *
 * Export collector dpo_pairs to JSONL for `human ml lora-persona`.
 * Sprint B C-loop (2026-05-24). */

#include "human/ml/lora_export.h"

#include "human/cli_commands.h"
#include "human/core/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !HU_IS_TEST && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

/* ── pure JSON escape ───────────────────────────────────────────────── */

size_t hu_lora_export_json_escape(const char *src, char *dst, size_t cap) {
    if (!dst || cap == 0)
        return 0;
    dst[0] = '\0';
    if (!src)
        return 0;
    size_t off = 0;
    while (*src && off + 2 < cap) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') {
            if (off + 3 >= cap)
                break;
            dst[off++] = '\\';
            dst[off++] = (char)c;
        } else if (c == '\n') {
            if (off + 3 >= cap)
                break;
            dst[off++] = '\\';
            dst[off++] = 'n';
        } else if (c == '\r') {
            if (off + 3 >= cap)
                break;
            dst[off++] = '\\';
            dst[off++] = 'r';
        } else if (c == '\t') {
            if (off + 3 >= cap)
                break;
            dst[off++] = '\\';
            dst[off++] = 't';
        } else if (c < 0x20) {
            if (off + 7 >= cap)
                break;
            int n = snprintf(dst + off, cap - off, "\\u%04x", c);
            if (n > 0)
                off += (size_t)n;
        } else {
            dst[off++] = (char)c;
        }
    }
    dst[off] = '\0';
    return off;
}

/* ── pure JSONL line render ─────────────────────────────────────────── */

size_t hu_lora_export_render_jsonl_line(const hu_lora_export_pair_t *pair, char *out, size_t cap) {
    if (!pair || !out || cap < 32)
        return 0;
    out[0] = '\0';
    /* Drop unusable rows. */
    if (!pair->prompt[0] || !pair->chosen[0])
        return 0;

    /* Escape each field into a generously-sized stack buffer (2× source
     * cap, since worst case every char gets a 2-char escape). */
    char esc_prompt[HU_LORA_EXPORT_PROMPT_MAX * 2 + 16];
    char esc_chosen[HU_LORA_EXPORT_RESP_MAX * 2 + 16];
    char esc_rejected[HU_LORA_EXPORT_RESP_MAX * 2 + 16];
    hu_lora_export_json_escape(pair->prompt, esc_prompt, sizeof(esc_prompt));
    hu_lora_export_json_escape(pair->chosen, esc_chosen, sizeof(esc_chosen));
    hu_lora_export_json_escape(pair->rejected, esc_rejected, sizeof(esc_rejected));

    int n;
    if (pair->rejected[0]) {
        n = snprintf(out, cap,
                     "{\"prompt\":\"%s\",\"chosen\":\"%s\",\"rejected\":\"%s\",\"ts\":%lld}",
                     esc_prompt, esc_chosen, esc_rejected, (long long)pair->timestamp);
    } else {
        /* Some pairs have no rejected sample (no alternative was sampled
         * at generation time). Emit a SFT-style row instead — many LoRA
         * trainers accept either shape; lora-persona reads the chosen
         * column unconditionally. */
        n = snprintf(out, cap, "{\"prompt\":\"%s\",\"chosen\":\"%s\",\"ts\":%lld}", esc_prompt,
                     esc_chosen, (long long)pair->timestamp);
    }
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}

/* ── pure KTO JSONL line render ─────────────────────────────────────── */

size_t hu_lora_export_render_kto_jsonl_line(const hu_lora_export_kto_t *row, char *out, size_t cap) {
    if (!row || !out || cap < 32)
        return 0;
    out[0] = '\0';
    /* Drop unusable rows — KTO needs both a prompt and the completion it
     * is labeling. */
    if (!row->prompt[0] || !row->completion[0])
        return 0;

    char esc_prompt[HU_LORA_EXPORT_PROMPT_MAX * 2 + 16];
    char esc_completion[HU_LORA_EXPORT_RESP_MAX * 2 + 16];
    hu_lora_export_json_escape(row->prompt, esc_prompt, sizeof(esc_prompt));
    hu_lora_export_json_escape(row->completion, esc_completion, sizeof(esc_completion));

    int n = snprintf(out, cap,
                     "{\"prompt\":\"%s\",\"completion\":\"%s\",\"label\":%s,\"ts\":%lld}",
                     esc_prompt, esc_completion, row->label ? "true" : "false",
                     (long long)row->timestamp);
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}

/* ── SQL exporter (gated; stubs off-platform) ───────────────────────── */

#if !HU_IS_TEST && defined(HU_ENABLE_SQLITE)

hu_error_t hu_lora_export_kto_signals(hu_allocator_t *alloc, const char *db_path,
                                      const char *out_file_path, int64_t since_unix,
                                      size_t *out_count) {
    if (!alloc || !db_path || !out_file_path || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    const char *sql = "SELECT prompt, response, label, timestamp FROM feedback_signals "
                      "WHERE timestamp >= ? ORDER BY timestamp ASC";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(st, 1, since_unix);

    FILE *fp = fopen(out_file_path, "w");
    if (!fp) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char line_buf[HU_LORA_EXPORT_PROMPT_MAX * 4 + HU_LORA_EXPORT_RESP_MAX * 4 + 256];
    size_t wrote = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        hu_lora_export_kto_t row;
        memset(&row, 0, sizeof(row));
        const unsigned char *p = sqlite3_column_text(st, 0);
        const unsigned char *c = sqlite3_column_text(st, 1);
        if (p)
            snprintf(row.prompt, sizeof(row.prompt), "%s", (const char *)p);
        if (c)
            snprintf(row.completion, sizeof(row.completion), "%s", (const char *)c);
        /* feedback_signals.label is INTEGER: 1 = desirable, 0 = undesirable. */
        row.label = sqlite3_column_int(st, 2) != 0;
        row.timestamp = sqlite3_column_int64(st, 3);

        size_t n = hu_lora_export_render_kto_jsonl_line(&row, line_buf, sizeof(line_buf));
        if (n == 0)
            continue;
        if (fwrite(line_buf, 1, n, fp) != n) {
            sqlite3_finalize(st);
            fclose(fp);
            sqlite3_close(db);
            return HU_ERR_IO;
        }
        fputc('\n', fp);
        wrote++;
    }
    sqlite3_finalize(st);
    fclose(fp);
    sqlite3_close(db);
    *out_count = wrote;
    return HU_OK;
}

hu_error_t hu_lora_export_dpo_pairs(hu_allocator_t *alloc, const char *db_path,
                                    const char *out_file_path, int64_t since_unix,
                                    size_t *out_count) {
    if (!alloc || !db_path || !out_file_path || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    const char *sql = "SELECT prompt, chosen, rejected, timestamp FROM dpo_pairs "
                      "WHERE timestamp >= ? ORDER BY timestamp ASC";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(st, 1, since_unix);

    FILE *fp = fopen(out_file_path, "w");
    if (!fp) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char line_buf[HU_LORA_EXPORT_PROMPT_MAX * 4 + HU_LORA_EXPORT_RESP_MAX * 4 + 256];
    size_t wrote = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        hu_lora_export_pair_t pair;
        memset(&pair, 0, sizeof(pair));
        const unsigned char *p = sqlite3_column_text(st, 0);
        const unsigned char *c = sqlite3_column_text(st, 1);
        const unsigned char *r = sqlite3_column_text(st, 2);
        if (p)
            snprintf(pair.prompt, sizeof(pair.prompt), "%s", (const char *)p);
        if (c)
            snprintf(pair.chosen, sizeof(pair.chosen), "%s", (const char *)c);
        if (r)
            snprintf(pair.rejected, sizeof(pair.rejected), "%s", (const char *)r);
        pair.timestamp = sqlite3_column_int64(st, 3);

        size_t n = hu_lora_export_render_jsonl_line(&pair, line_buf, sizeof(line_buf));
        if (n == 0)
            continue;
        if (fwrite(line_buf, 1, n, fp) != n) {
            sqlite3_finalize(st);
            fclose(fp);
            sqlite3_close(db);
            return HU_ERR_IO;
        }
        fputc('\n', fp);
        wrote++;
    }
    sqlite3_finalize(st);
    fclose(fp);
    sqlite3_close(db);
    *out_count = wrote;
    return HU_OK;
}

#else

hu_error_t hu_lora_export_dpo_pairs(hu_allocator_t *alloc, const char *db_path,
                                    const char *out_file_path, int64_t since_unix,
                                    size_t *out_count) {
    (void)alloc;
    (void)db_path;
    (void)out_file_path;
    (void)since_unix;
    if (out_count)
        *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_lora_export_kto_signals(hu_allocator_t *alloc, const char *db_path,
                                      const char *out_file_path, int64_t since_unix,
                                      size_t *out_count) {
    (void)alloc;
    (void)db_path;
    (void)out_file_path;
    (void)since_unix;
    if (out_count)
        *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

/* ── CLI: human export-dpo ──────────────────────────────────────────── */

static const char *kExportDpoUsage =
    "Usage: human export-dpo [--db <path>] [--out <jsonl>] [--since-days N]\n"
    "  --db <path>          dpo_pairs SQLite DB (default ~/.human/dpo_pairs.db)\n"
    "  --out <jsonl>        output JSONL path (default ~/.human/lora-pairs.jsonl)\n"
    "  --since-days N       export only pairs newer than N days (default 30; 0 = all)\n"
    "\n"
    "Writes one JSON object per line, suitable for `mlx_lm.lora --data`.\n"
    "See docs/guides/m3-bridge-runbook.md for the end-to-end fine-tune flow.\n";

hu_error_t cmd_export_dpo(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    const char *db_path = NULL;
    const char *out_path = NULL;
    int since_days = 30;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("%s", kExportDpoUsage);
            return HU_OK;
        } else if (strcmp(a, "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(a, "--since-days") == 0 && i + 1 < argc) {
            since_days = atoi(argv[++i]);
            if (since_days < 0)
                since_days = 0;
        }
    }

    char db_buf[512];
    char out_buf[512];
    if (!db_path) {
        const char *home = getenv("HOME");
        if (home && home[0] && snprintf(db_buf, sizeof(db_buf), "%s/.human/dpo_pairs.db", home) > 0)
            db_path = db_buf;
    }
    if (!out_path) {
        const char *home = getenv("HOME");
        if (home && home[0] &&
            snprintf(out_buf, sizeof(out_buf), "%s/.human/lora-pairs.jsonl", home) > 0)
            out_path = out_buf;
    }
    if (!db_path || !out_path) {
        fprintf(stderr, "could not resolve default paths\n");
        return HU_ERR_IO;
    }

    int64_t since_unix = 0;
    if (since_days > 0)
        since_unix = (int64_t)time(NULL) - ((int64_t)since_days * 86400);

    size_t count = 0;
    hu_error_t err = hu_lora_export_dpo_pairs(alloc, db_path, out_path, since_unix, &count);
    if (err == HU_ERR_NOT_SUPPORTED) {
        hu_log_error(
            "export-dpo", NULL,
            "export not supported on this build (need HU_ENABLE_SQLITE; not a test build)");
        return err;
    }
    if (err != HU_OK) {
        hu_log_error("export-dpo", NULL, "export failed: %s", hu_error_string(err));
        return err;
    }
    printf("Wrote %zu pairs to %s (since=%d days from %s)\n", count, out_path, since_days, db_path);
    return HU_OK;
}

/* ── CLI: human export-kto ──────────────────────────────────────────── */

static const char *kExportKtoUsage =
    "Usage: human export-kto [--db <path>] [--out <jsonl>] [--since-days N]\n"
    "  --db <path>          memory SQLite DB with feedback_signals (default ~/.human/memory.db)\n"
    "  --out <jsonl>        output KTO JSONL path (default ~/.human/lora-kto.jsonl)\n"
    "  --since-days N       export only signals newer than N days (default 30; 0 = all)\n"
    "\n"
    "Exports SINGLE-SIDED reaction feedback as KTO rows {prompt, completion, label}.\n"
    "This is the data DPO drops — feed it to `human ml kto-train --data <jsonl>`.\n";

hu_error_t cmd_export_kto(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    const char *db_path = NULL;
    const char *out_path = NULL;
    int since_days = 30;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("%s", kExportKtoUsage);
            return HU_OK;
        } else if (strcmp(a, "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(a, "--since-days") == 0 && i + 1 < argc) {
            since_days = atoi(argv[++i]);
            if (since_days < 0)
                since_days = 0;
        }
    }

    char db_buf[512];
    char out_buf[512];
    if (!db_path) {
        const char *home = getenv("HOME");
        if (home && home[0] && snprintf(db_buf, sizeof(db_buf), "%s/.human/memory.db", home) > 0)
            db_path = db_buf;
    }
    if (!out_path) {
        const char *home = getenv("HOME");
        if (home && home[0] &&
            snprintf(out_buf, sizeof(out_buf), "%s/.human/lora-kto.jsonl", home) > 0)
            out_path = out_buf;
    }
    if (!db_path || !out_path) {
        fprintf(stderr, "could not resolve default paths\n");
        return HU_ERR_IO;
    }

    int64_t since_unix = 0;
    if (since_days > 0)
        since_unix = (int64_t)time(NULL) - ((int64_t)since_days * 86400);

    size_t count = 0;
    hu_error_t err = hu_lora_export_kto_signals(alloc, db_path, out_path, since_unix, &count);
    if (err == HU_ERR_NOT_SUPPORTED) {
        hu_log_error(
            "export-kto", NULL,
            "export not supported on this build (need HU_ENABLE_SQLITE; not a test build)");
        return err;
    }
    if (err != HU_OK) {
        hu_log_error("export-kto", NULL, "export failed: %s", hu_error_string(err));
        return err;
    }
    printf("Wrote %zu KTO signals to %s (since=%d days from %s)\n", count, out_path, since_days,
           db_path);
    return HU_OK;
}
