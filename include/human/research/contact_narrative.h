/* include/human/research/contact_narrative.h
 *
 * Long-horizon contact narratives — Sprint B Story 4 (2026-05-19).
 *
 * `human research --contact <handle>` walks 5+ years of chat.db
 * history for one contact and produces a per-year narrative summary
 * ("You and Alice's relationship started with project work in 2022;
 * shifted to hiking + climbing in 2024…"), written to
 * ~/.human/contacts/<canonical>.md.
 *
 * Architecture — pure layers + thin glue:
 *   1. Scanner    : SQL query that buckets messages by year + counts
 *                   them. Returns a fixed-size bucket array.
 *   2. Prompt     : Per-year LLM prompt builder + final synthesis
 *                   prompt builder. Pure, deterministic.
 *   3. Renderer   : Format year-summaries as Markdown for the output
 *                   file. Pure.
 *   4. CLI/exec   : Wires scanner → for each year, generate summary
 *                   via provider → synthesize → render → write file.
 *
 * Persistent state: the markdown file itself is the state. Re-runs
 * detect existing years via a "## Year YYYY" heading scan and skip
 * already-summarized years (idempotent).
 */
#ifndef HU_RESEARCH_CONTACT_NARRATIVE_H
#define HU_RESEARCH_CONTACT_NARRATIVE_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard caps. 30 years of history × 1 entry per year is plenty. */
#define HU_CONTACT_NARRATIVE_MAX_YEARS   30
#define HU_CONTACT_NARRATIVE_HANDLE_MAX  128
#define HU_CONTACT_NARRATIVE_SUMMARY_MAX 1024
#define HU_CONTACT_NARRATIVE_PROMPT_MAX  8192

typedef struct hu_contact_narrative_year_bucket {
    int16_t year;      /* e.g. 2024 */
    int32_t msg_count; /* total messages exchanged that year */
    int32_t from_them; /* messages they sent */
    int32_t from_me;   /* messages user sent */
    int64_t first_ts;  /* unix; earliest message that year */
    int64_t last_ts;   /* unix; latest message that year */
} hu_contact_narrative_year_bucket_t;

typedef struct hu_contact_narrative_scan_result {
    hu_contact_narrative_year_bucket_t buckets[HU_CONTACT_NARRATIVE_MAX_YEARS];
    size_t bucket_count;
} hu_contact_narrative_scan_result_t;

/* Walk chat.db and produce a per-year bucket for the contact.
 * Returns HU_ERR_NOT_SUPPORTED on non-Apple builds or when chat.db
 * is not accessible. */
hu_error_t hu_contact_narrative_scan(const char *chat_db_path, const char *contact_handle,
                                     hu_contact_narrative_scan_result_t *out);

/* Build the per-year summary prompt. Pure, no I/O. The prompt asks
 * the LLM to produce a 2-3 sentence summary characterizing the
 * relationship that year (topics, tone, frequency).
 *
 * `sample_messages` is an optional caller-supplied excerpt of actual
 * message text (newline-delimited). Pass NULL when no sample is
 * available; the prompt then asks the model to summarize from
 * statistics alone (less useful but never crashes). */
size_t hu_contact_narrative_build_year_prompt(const char *contact_handle, int year,
                                              const hu_contact_narrative_year_bucket_t *bucket,
                                              const char *sample_messages, char *out, size_t cap);

/* Build the final synthesis prompt that takes per-year summaries and
 * asks the LLM to weave them into a flowing narrative. Pure. */
size_t hu_contact_narrative_build_synthesis_prompt(const char *contact_handle,
                                                   const char *const *year_summaries,
                                                   const int *years, size_t count, char *out,
                                                   size_t cap);

/* Render a markdown document from the scan result + per-year
 * summaries. Pure; writes to caller-owned buffer. Output shape:
 *
 *   # <contact>
 *
 *   Generated: <iso8601>
 *   Total years: <n>
 *
 *   ## Year 2022
 *   <summary>
 *
 *   ## Year 2023
 *   <summary>
 *
 *   ...
 *
 *   ## Synthesis
 *   <synthesis>
 */
size_t hu_contact_narrative_render_markdown(const char *contact_handle,
                                            const hu_contact_narrative_scan_result_t *scan,
                                            const char *const *year_summaries,
                                            const char *synthesis, int64_t generated_at_unix,
                                            char *out, size_t cap);

/* Detect which years are already covered by an existing markdown
 * file (idempotent resume). Walks the file looking for "## Year YYYY"
 * headings. Returns the count of years found via *out_count and
 * fills *out_years (caller-allocated, capacity HU_CONTACT_NARRATIVE_MAX_YEARS). */
hu_error_t hu_contact_narrative_parse_existing_years(const char *file_path, int *out_years,
                                                     size_t *out_count);

/* Resolve the default per-contact output path:
 *   ~/.human/contacts/<sanitized_handle>.md
 * where sanitized strips characters NOT in [A-Za-z0-9._+-] (replaces
 * with '_'). Returns bytes written, or 0 on error.
 *
 * Creates the ~/.human/contacts/ directory (mode 0700) if missing. */
size_t hu_contact_narrative_default_path(const char *contact_handle, char *out, size_t cap);

/* CLI: `human narrate --contact <handle> [--db <path>] [--out <path>]`.
 * Wires scanner + LLM generator + markdown renderer + file write.
 * (Named `narrate` not `research` to avoid colliding with the existing
 * feed-research subcommand in src/cli_commands.c.) */
hu_error_t cmd_narrate(hu_allocator_t *alloc, int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif /* HU_RESEARCH_CONTACT_NARRATIVE_H */
