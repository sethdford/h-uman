/* src/research/contact_narrative.c
 *
 * Long-horizon contact narratives. See header for contract.
 * Sprint B Story 4 (docs/plans/2026-05-19-sprint-backlog.md). */

#include "human/research/contact_narrative.h"

#include "human/config.h"
#include "human/core/log.h"
#include "human/memory/personal_model.h"
#include "human/providers/factory.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include "human/channels/imessage.h" /* hu_imessage_extract_attributed_body */
#include <sqlite3.h>
#endif

#define HU_MAC_EPOCH_OFFSET 978307200LL

/* ── sanitize handle for filename ───────────────────────────────────── */

size_t hu_contact_narrative_default_path(const char *contact_handle, char *out, size_t cap) {
    if (!contact_handle || !*contact_handle || !out || cap < 16)
        return 0;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return 0;

    /* Ensure the parent dir exists (0700 — same as ~/.human/). */
    char dir[512];
    int dn = snprintf(dir, sizeof(dir), "%s/.human/contacts", home);
    if (dn < 0 || (size_t)dn >= sizeof(dir))
        return 0;
    (void)mkdir(dir, 0700); /* ignore EEXIST */

    /* Sanitize: keep [A-Za-z0-9._+-]; replace others with '_'. */
    char sani[HU_CONTACT_NARRATIVE_HANDLE_MAX];
    size_t si = 0;
    for (size_t i = 0; contact_handle[i] && si + 1 < sizeof(sani); i++) {
        char c = contact_handle[i];
        if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '+' || c == '-')
            sani[si++] = c;
        else
            sani[si++] = '_';
    }
    sani[si] = '\0';
    if (si == 0)
        return 0;

    int n = snprintf(out, cap, "%s/%s.md", dir, sani);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── SQL scan (gated; stubbed off-platform) ─────────────────────────── */

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)

hu_error_t hu_contact_narrative_scan(const char *chat_db_path, const char *contact_handle,
                                     hu_contact_narrative_scan_result_t *out) {
    if (!chat_db_path || !contact_handle || !*contact_handle || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(chat_db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    /* Group by year: strftime('%Y', date/1e9 + 978307200, 'unixepoch').
     * That puts each message's local year as the bucket key. */
    const char *sql = "SELECT CAST(strftime('%Y', m.date/1000000000 + 978307200, 'unixepoch') AS "
                      "INTEGER) AS yr, "
                      " COUNT(*) AS total, "
                      " SUM(CASE WHEN m.is_from_me=0 THEN 1 ELSE 0 END) AS from_them, "
                      " SUM(CASE WHEN m.is_from_me=1 THEN 1 ELSE 0 END) AS from_me, "
                      " MIN(m.date) AS first_d, "
                      " MAX(m.date) AS last_d "
                      "FROM message m JOIN handle h ON h.ROWID = m.handle_id "
                      "WHERE h.id = ? AND m.associated_message_type = 0 "
                      "GROUP BY yr ORDER BY yr ASC";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_text(st, 1, contact_handle, -1, SQLITE_STATIC);

    while (sqlite3_step(st) == SQLITE_ROW && out->bucket_count < HU_CONTACT_NARRATIVE_MAX_YEARS) {
        hu_contact_narrative_year_bucket_t *b = &out->buckets[out->bucket_count++];
        b->year = (int16_t)sqlite3_column_int(st, 0);
        b->msg_count = sqlite3_column_int(st, 1);
        b->from_them = sqlite3_column_int(st, 2);
        b->from_me = sqlite3_column_int(st, 3);
        int64_t first_d = sqlite3_column_int64(st, 4);
        int64_t last_d = sqlite3_column_int64(st, 5);
        b->first_ts = (first_d / 1000000000LL) + HU_MAC_EPOCH_OFFSET;
        b->last_ts = (last_d / 1000000000LL) + HU_MAC_EPOCH_OFFSET;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return HU_OK;
}

#else

hu_error_t hu_contact_narrative_scan(const char *chat_db_path, const char *contact_handle,
                                     hu_contact_narrative_scan_result_t *out) {
    (void)chat_db_path;
    (void)contact_handle;
    if (out)
        memset(out, 0, sizeof(*out));
    return HU_ERR_NOT_SUPPORTED;
}

#endif

/* ── safe string helpers ────────────────────────────────────────────── */

static void cn_append(char *dst, size_t cap, size_t *off, const char *s) {
    if (!dst || cap == 0 || *off + 1 >= cap || !s)
        return;
    size_t avail = cap - *off - 1;
    size_t i = 0;
    while (s[i] && i < avail) {
        dst[*off + i] = s[i];
        i++;
    }
    *off += i;
    dst[*off] = '\0';
}

static void cn_append_fmt(char *dst, size_t cap, size_t *off, const char *fmt, ...) {
    if (!dst || cap == 0 || *off + 1 >= cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    size_t avail = cap - *off;
    int n = vsnprintf(dst + *off, avail, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t added = (size_t)n < avail ? (size_t)n : avail - 1;
        *off += added;
    }
}

/* ── prompt builders (pure) ─────────────────────────────────────────── */

size_t hu_contact_narrative_build_year_prompt(const char *contact_handle, int year,
                                              const hu_contact_narrative_year_bucket_t *bucket,
                                              const char *sample_messages, char *out, size_t cap) {
    if (!out || cap < 2 || !contact_handle || !*contact_handle || !bucket)
        return 0;
    out[0] = '\0';
    size_t n = 0;
    cn_append_fmt(out, cap, &n,
                  "Summarize the relationship between the user and %s during %d in 2-3 "
                  "sentences. Focus on topics, tone, and frequency. Do not invent specifics "
                  "the data doesn't support.\n\n",
                  contact_handle, year);
    cn_append_fmt(out, cap, &n,
                  "Statistics for %d:\n"
                  "  Total messages: %d\n"
                  "  From %s: %d\n"
                  "  From user: %d\n",
                  year, bucket->msg_count, contact_handle, bucket->from_them, bucket->from_me);
    if (sample_messages && *sample_messages) {
        cn_append(out, cap, &n, "\nSample messages (oldest first, abbreviated):\n");
        cn_append(out, cap, &n, sample_messages);
        cn_append(out, cap, &n, "\n");
    } else {
        cn_append(out, cap, &n,
                  "\n(No message text available — summarize from statistics only.)\n");
    }
    cn_append(out, cap, &n, "\nSummary:");
    return n;
}

size_t hu_contact_narrative_build_synthesis_prompt(const char *contact_handle,
                                                   const char *const *year_summaries,
                                                   const int *years, size_t count, char *out,
                                                   size_t cap) {
    if (!out || cap < 2 || !contact_handle || !*contact_handle || !year_summaries || !years ||
        count == 0)
        return 0;
    out[0] = '\0';
    size_t n = 0;
    cn_append_fmt(out, cap, &n,
                  "Below are year-by-year summaries of the user's relationship with %s. "
                  "Weave them into ONE flowing 4-6 sentence narrative that captures the "
                  "arc of how the relationship evolved. Avoid year-by-year recap — show "
                  "the trajectory.\n\n",
                  contact_handle);
    for (size_t i = 0; i < count; i++) {
        cn_append_fmt(out, cap, &n, "## Year %d\n", years[i]);
        cn_append(out, cap, &n, year_summaries[i] ? year_summaries[i] : "(no summary)");
        cn_append(out, cap, &n, "\n\n");
    }
    cn_append(out, cap, &n, "Narrative:");
    return n;
}

/* ── markdown renderer (pure) ───────────────────────────────────────── */

static void format_iso8601(int64_t ts, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    time_t t = (time_t)ts;
    struct tm gm;
#if defined(_WIN32)
    gmtime_s(&gm, &t);
#else
    gmtime_r(&t, &gm);
#endif
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &gm);
}

size_t hu_contact_narrative_render_markdown(const char *contact_handle,
                                            const hu_contact_narrative_scan_result_t *scan,
                                            const char *const *year_summaries,
                                            const char *synthesis, int64_t generated_at_unix,
                                            char *out, size_t cap) {
    if (!out || cap < 32 || !contact_handle || !*contact_handle || !scan)
        return 0;
    out[0] = '\0';
    size_t n = 0;

    char iso[32] = {0};
    format_iso8601(generated_at_unix, iso, sizeof(iso));

    cn_append_fmt(out, cap, &n,
                  "# %s\n\n"
                  "Generated: %s\n"
                  "Total years: %zu\n\n",
                  contact_handle, iso, scan->bucket_count);

    for (size_t i = 0; i < scan->bucket_count; i++) {
        const hu_contact_narrative_year_bucket_t *b = &scan->buckets[i];
        cn_append_fmt(out, cap, &n,
                      "## Year %d\n"
                      "_%d messages (%d from them, %d from you)_\n\n",
                      b->year, b->msg_count, b->from_them, b->from_me);
        if (year_summaries && year_summaries[i]) {
            cn_append(out, cap, &n, year_summaries[i]);
            cn_append(out, cap, &n, "\n\n");
        } else {
            cn_append(out, cap, &n, "_(no summary)_\n\n");
        }
    }

    if (synthesis && *synthesis) {
        cn_append(out, cap, &n, "## Synthesis\n");
        cn_append(out, cap, &n, synthesis);
        cn_append(out, cap, &n, "\n");
    }
    return n;
}

/* ── resume: parse "## Year YYYY" headings from existing file ──────── */

hu_error_t hu_contact_narrative_parse_existing_years(const char *file_path, int *out_years,
                                                     size_t *out_count) {
    if (!file_path || !out_years || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    FILE *fp = fopen(file_path, "r");
    if (!fp)
        return HU_ERR_NOT_FOUND;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Match "## Year YYYY" — accept trailing whitespace/newline. */
        int year = 0;
        if (sscanf(line, "## Year %d", &year) == 1 && year >= 1900 && year <= 9999) {
            if (*out_count < HU_CONTACT_NARRATIVE_MAX_YEARS) {
                out_years[*out_count] = year;
                (*out_count)++;
            }
        }
    }
    fclose(fp);
    return HU_OK;
}

/* ── CLI subcommand ─────────────────────────────────────────────────── */

static const char *kNarrateUsage =
    "Usage: human narrate --contact <handle> [--db <path>] [--out <path>] [--no-llm]\n"
    "  --contact <handle>   Recipient handle (required)\n"
    "  --db <path>          chat.db path (default ~/Library/Messages/chat.db)\n"
    "  --out <path>         Output markdown (default ~/.human/contacts/<handle>.md)\n"
    "  --no-llm             Skip LLM summarization; render stats-only markdown\n";

hu_error_t cmd_narrate(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    const char *contact = NULL;
    const char *db = NULL;
    const char *out_path = NULL;
    bool want_help = false;
    bool no_llm = false;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            want_help = true;
        } else if (strcmp(a, "--contact") == 0 && i + 1 < argc) {
            contact = argv[++i];
        } else if (strcmp(a, "--db") == 0 && i + 1 < argc) {
            db = argv[++i];
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(a, "--no-llm") == 0) {
            no_llm = true;
        }
    }

    if (want_help || !contact) {
        printf("%s", kNarrateUsage);
        return want_help ? HU_OK : HU_ERR_INVALID_ARGUMENT;
    }

    /* Default chat.db. */
    char default_db[512];
    if (!db) {
        const char *home = getenv("HOME");
        if (home && home[0] &&
            snprintf(default_db, sizeof(default_db), "%s/Library/Messages/chat.db", home) > 0) {
            db = default_db;
        }
    }

    /* Default output path. */
    char default_out[512];
    if (!out_path) {
        if (hu_contact_narrative_default_path(contact, default_out, sizeof(default_out)) > 0)
            out_path = default_out;
    }
    if (!out_path) {
        hu_log_error("research", NULL, "could not resolve output path for contact \"%s\"", contact);
        return HU_ERR_IO;
    }

    /* Scan chat.db. */
    hu_contact_narrative_scan_result_t scan;
    hu_error_t serr = hu_contact_narrative_scan(db, contact, &scan);
    if (serr == HU_ERR_NOT_SUPPORTED) {
        hu_log_error("research", NULL,
                     "chat.db scan not supported on this build/platform (need macOS + SQLite)");
        return serr;
    }
    if (serr != HU_OK) {
        hu_log_error("research", NULL, "chat.db scan failed: %s", hu_error_string(serr));
        return serr;
    }
    if (scan.bucket_count == 0) {
        hu_log_info("research", NULL, "no messages found for contact \"%s\" in %s", contact, db);
        return HU_OK;
    }

    /* Existing years (resume support). */
    int existing[HU_CONTACT_NARRATIVE_MAX_YEARS];
    size_t existing_count = 0;
    (void)hu_contact_narrative_parse_existing_years(out_path, existing, &existing_count);

    /* Per-year summaries: in --no-llm mode, write stats only. With LLM,
     * call provider once per year. */
    static const char *kEmptySummary = "(no summary yet)";
    const char *summaries[HU_CONTACT_NARRATIVE_MAX_YEARS];
    char *summary_owned[HU_CONTACT_NARRATIVE_MAX_YEARS] = {0};
    for (size_t i = 0; i < scan.bucket_count; i++)
        summaries[i] = kEmptySummary;

    char *synth_owned = NULL;
    if (!no_llm) {
        /* Load provider once. */
        hu_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        hu_error_t cerr = hu_config_load(alloc, &cfg);
        if (cerr == HU_OK) {
            hu_provider_t provider;
            memset(&provider, 0, sizeof(provider));
            hu_error_t perr = hu_provider_create_default(alloc, &cfg, &provider);
            if (perr == HU_OK && provider.vtable && provider.vtable->chat_with_system) {
                static const char *kSystem =
                    "You are a careful biographer. Output only the requested summary text — no "
                    "lists, no headers, no preamble.";
                const char *model_id = cfg.default_model;
                size_t model_id_len = model_id ? strlen(model_id) : 0;
                for (size_t i = 0; i < scan.bucket_count; i++) {
                    int yr = scan.buckets[i].year;
                    /* Skip if year is already in existing file. */
                    bool skip = false;
                    for (size_t e = 0; e < existing_count; e++) {
                        if (existing[e] == yr) {
                            skip = true;
                            break;
                        }
                    }
                    if (skip)
                        continue;
                    char prompt[HU_CONTACT_NARRATIVE_PROMPT_MAX];
                    hu_contact_narrative_build_year_prompt(contact, yr, &scan.buckets[i], NULL,
                                                           prompt, sizeof(prompt));
                    char *resp = NULL;
                    size_t resp_len = 0;
                    hu_error_t ge = provider.vtable->chat_with_system(
                        provider.ctx, alloc, kSystem, strlen(kSystem), prompt, strlen(prompt),
                        model_id, model_id_len, 0.5, &resp, &resp_len);
                    if (ge == HU_OK && resp && resp_len > 0) {
                        /* Take ownership; cap at SUMMARY_MAX. */
                        size_t copy = resp_len > HU_CONTACT_NARRATIVE_SUMMARY_MAX
                                          ? HU_CONTACT_NARRATIVE_SUMMARY_MAX
                                          : resp_len;
                        summary_owned[i] = (char *)malloc(copy + 1);
                        if (summary_owned[i]) {
                            memcpy(summary_owned[i], resp, copy);
                            summary_owned[i][copy] = '\0';
                            summaries[i] = summary_owned[i];
                        }
                    }
                    if (resp)
                        alloc->free(alloc->ctx, resp, resp_len + 1);
                }
                /* Synthesis pass over all populated summaries. */
                {
                    const char *summary_ptrs[HU_CONTACT_NARRATIVE_MAX_YEARS];
                    int year_ints[HU_CONTACT_NARRATIVE_MAX_YEARS];
                    for (size_t i = 0; i < scan.bucket_count; i++) {
                        summary_ptrs[i] = summaries[i];
                        year_ints[i] = scan.buckets[i].year;
                    }
                    char synth_prompt[HU_CONTACT_NARRATIVE_PROMPT_MAX];
                    hu_contact_narrative_build_synthesis_prompt(contact, summary_ptrs, year_ints,
                                                                scan.bucket_count, synth_prompt,
                                                                sizeof(synth_prompt));
                    char *sresp = NULL;
                    size_t sresp_len = 0;
                    hu_error_t se = provider.vtable->chat_with_system(
                        provider.ctx, alloc, kSystem, strlen(kSystem), synth_prompt,
                        strlen(synth_prompt), model_id, model_id_len, 0.5, &sresp, &sresp_len);
                    if (se == HU_OK && sresp && sresp_len > 0) {
                        synth_owned = strdup(sresp);
                    }
                    if (sresp)
                        alloc->free(alloc->ctx, sresp, sresp_len + 1);
                }
                if (provider.vtable->deinit)
                    provider.vtable->deinit(provider.ctx, alloc);
            }
            hu_config_deinit(&cfg);
        }
    }

    /* Render markdown. */
    char *md = (char *)malloc(64 * 1024);
    if (!md) {
        for (size_t i = 0; i < HU_CONTACT_NARRATIVE_MAX_YEARS; i++)
            free(summary_owned[i]);
        free(synth_owned);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_contact_narrative_render_markdown(contact, &scan, summaries, synth_owned,
                                         (int64_t)time(NULL), md, 64 * 1024);

    FILE *fp = fopen(out_path, "w");
    hu_error_t result = HU_OK;
    if (!fp) {
        hu_log_error("research", NULL, "could not write %s", out_path);
        result = HU_ERR_IO;
    } else {
        fputs(md, fp);
        fclose(fp);
        printf("Wrote %zu-year narrative for %s to %s\n", scan.bucket_count, contact, out_path);
    }

    free(md);
    free(synth_owned);
    for (size_t i = 0; i < HU_CONTACT_NARRATIVE_MAX_YEARS; i++)
        free(summary_owned[i]);
    return result;
}
