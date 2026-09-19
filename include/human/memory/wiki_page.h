#ifndef HU_MEMORY_WIKI_PAGE_H
#define HU_MEMORY_WIKI_PAGE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Per-contact wiki page (better-than-human item 4, sleep-time consolidation)
 *
 * scripts/consolidate_wiki.py compiles <state>/wiki/<contact_id>.md nightly
 * from the insight, prospective and persona stores; every line carries a
 * provenance tag and the page is linted before it is published. The memory
 * loader reads a budgeted HEAD of that page behind HU_WIKI_HEAD
 * (off|shadow|live) and, when live, shrinks the raw recall block by the same
 * bytes so the prompt does not grow. This module is the pure half: id
 * safety, the line-boundary slice, the recall offset, and the file read
 * through hu_paths_state (never a hand-formatted ~/.human path).
 * ────────────────────────────────────────────────────────────────────────── */

/* Pages are read in full up to this many bytes, then sliced to the budget. */
#define HU_WIKI_PAGE_MAX_BYTES 8192

/* A contact id becomes a file name: only [A-Za-z0-9+@._-], 1..96 bytes, not
 * starting with '.', and never containing "..". Anything else is refused. */
bool hu_wiki_contact_id_is_safe(const char *contact_id, size_t len);

/* Bytes of `page` to keep under `budget`: the whole page when it fits, else
 * the longest prefix that ends on a '\n' within the first `budget` bytes
 * (0 when no line fits). Never splits a line. */
size_t hu_wiki_page_slice(const char *page, size_t page_len, size_t budget);

/* Recall byte cap once a LIVE page of `wiki_len` bytes is in the prompt: the
 * cap drops by the page's bytes so the prompt gets no bigger, floored at half
 * the original cap so raw recall never vanishes. wiki_len 0 = unchanged. */
size_t hu_wiki_recall_cap(size_t max_context_chars, size_t wiki_len);

/* Read the page head for a contact: HU_ERR_INVALID_ARGUMENT for an unsafe id,
 * HU_ERR_NOT_FOUND when there is no page, HU_OK with *out NULL when the page
 * is empty or nothing fits `budget`. Trailing newlines are stripped. Caller
 * frees *out with alloc->free(ctx, out, *out_len + 1). */
hu_error_t hu_wiki_page_read(hu_allocator_t *alloc, const char *contact_id, size_t cid_len,
                             size_t budget, char **out, size_t *out_len);

#endif /* HU_MEMORY_WIKI_PAGE_H */
