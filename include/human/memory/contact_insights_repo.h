/* include/human/memory/contact_insights_repo.h
 *
 * Per-contact insight stream (docs/plans/2026-09-06-better-than-human item 3,
 * DualMem-shaped): short notes of what Seth would ACTUALLY remember and bring
 * up with a given person — names, places, running threads, plans, inside
 * references — kept beside the raw fact store, persona-conditioned at
 * extraction time, and rendered into the prompt as a compact block.
 *
 * Why a separate stream: the n=40 human gate's residual tell is "generic where
 * Seth is specific" (2026-07-27), and the 2026-09-06 specificity baseline puts
 * the daemon at 1.10 specific tokens per reply against Seth's 2.13. The raw
 * `memories` recall is fact-shaped ("### Memory: key ... (stored: ts)"); this
 * is memory-shaped the way a person's is.
 *
 * Table `contact_insights` lives in the sqlite memory db. This repo owns its
 * DDL. Rows are written by the nightly extractor (scripts/insight_stream.py,
 * local GLM over the contact's recent turns) and read by the memory loader
 * behind HU_INSIGHT_STREAM (off | shadow | live, default off — see
 * .claude/rules/feature-gate-requires-measurement.md). Domain code depends on
 * hu_memory_t, never on sqlite3. */
#ifndef HU_MEMORY_CONTACT_INSIGHTS_REPO_H
#define HU_MEMORY_CONTACT_INSIGHTS_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"

#include <stddef.h>
#include <stdint.h>

/* Create the table + indexes if missing. Idempotent. HU_ERR_NOT_SUPPORTED on a
 * non-sqlite backend. */
hu_error_t hu_contact_insights_ensure_schema(hu_memory_t *mem);

/* Insert one live insight. `kind` is free text ("fact", "thread", "plan",
 * "preference", "inside_ref"); `source` is provenance (e.g. "extractor:v1").
 * as_of_ms = when the insight became true (0 = unknown). */
hu_error_t hu_contact_insights_add(hu_memory_t *mem, const char *contact_id, size_t contact_id_len,
                                   const char *kind, const char *insight, double confidence,
                                   int64_t as_of_ms, const char *source, int64_t *out_id);

/* Mark an insight retired (no longer rendered). */
hu_error_t hu_contact_insights_retire(hu_memory_t *mem, int64_t id, int64_t retired_at_ms);

/* Render the live insights for a contact as prompt lines:
 *   - <insight> (as of Mon YYYY)\n
 * newest as_of first, ties by id desc, confidence >= min_confidence, at most
 * `max_items` rows and `max_bytes` bytes (a row that would cross the byte cap
 * is dropped whole, never cut mid-line). *out is allocated with `alloc`
 * (*out_len + 1 bytes); on no rows sets *out = NULL, *out_len = 0, HU_OK. */
hu_error_t hu_contact_insights_render(hu_memory_t *mem, hu_allocator_t *alloc,
                                      const char *contact_id, size_t contact_id_len,
                                      size_t max_items, size_t max_bytes, double min_confidence,
                                      char **out, size_t *out_len);

/* How many of the newest live insights render_for_query weighs. */
#define HU_CONTACT_INSIGHTS_CANDIDATES 40

/* Choose at most `max_pick` of `n` insights (newest first) for a reply to
 * `query`: the ones sharing a content word with the message come first, most
 * shared words first, ties keeping recency; the rest of the slots go to the
 * newest of the others, for what is going on with them now. Writes candidate
 * indices to out_idx and returns how many. A NULL/empty query picks the
 * newest. Pure: no I/O. */
size_t hu_contact_insights_select(const char *const *insights, size_t n, const char *query,
                                  size_t query_len, size_t max_pick, size_t *out_idx);

/* hu_contact_insights_render, but the rows are chosen by
 * hu_contact_insights_select from the newest HU_CONTACT_INSIGHTS_CANDIDATES
 * against `query` (the incoming message) and rendered in that order. */
hu_error_t hu_contact_insights_render_for_query(hu_memory_t *mem, hu_allocator_t *alloc,
                                                const char *contact_id, size_t contact_id_len,
                                                const char *query, size_t query_len,
                                                size_t max_items, size_t max_bytes,
                                                double min_confidence, char **out, size_t *out_len);

/* The contact's live insights that became true at or after since_ms (as_of,
 * else created_at), newest first, joined with '\n' — at most 200 rows, with
 * the same confidence floor and curator_wide rule as the rendered block, so
 * "covered" means covered by notes the model can be shown. For deciding what
 * has gone unmentioned lately, not for the prompt. *out is allocated with
 * `alloc` (*out_len + 1 bytes); none: *out = NULL, HU_OK. */
hu_error_t hu_contact_insights_recent_text(hu_memory_t *mem, hu_allocator_t *alloc,
                                           const char *contact_id, size_t contact_id_len,
                                           int64_t since_ms, double min_confidence, char **out,
                                           size_t *out_len);

#endif /* HU_MEMORY_CONTACT_INSIGHTS_REPO_H */
