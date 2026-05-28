#ifndef HU_MEMORY_CROSS_CHANNEL_H
#define HU_MEMORY_CROSS_CHANNEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

typedef enum {
    HU_XCHAN_ACL_ALLOW = 0,
    HU_XCHAN_ACL_DENY,
    HU_XCHAN_ACL_DENY_UNKNOWN,
} hu_xchan_acl_decision_t;

typedef struct hu_cross_channel_item {
    enum {
        HU_XCHAN_FACT = 0,
        HU_XCHAN_REFLECTION_PATTERN,
    } source_type;
    char item_id[64];
    char *text;
    size_t text_len;
    char origin_channel[32];
    char origin_contact_id[64];
    char origin_relationship_type[32];
    int64_t observed_at_ms;
    double confidence;
} hu_cross_channel_item_t;

/* Pure predicate — no DB, no allocator, no LLM. Testable in isolation
 * per ~/.claude/rules/security-predicate-extraction.md. */
hu_xchan_acl_decision_t
hu_cross_channel_acl_check(const hu_persona_t *persona,
                           const char *origin_relationship_type, /* may be NULL */
                           const char *turn_relationship_type);  /* may be NULL */

/* Filter stage: applies ACL and compacts items in-place */
hu_error_t hu_cross_channel_filter(const hu_persona_t *persona, const char *turn_relationship_type,
                                   hu_cross_channel_item_t *items, size_t *count);

/* Collect stage: reads facts + reflection patterns from DB
 * Constructs cross-channel items from personal_model facts and reflection_patterns table.
 * Returns allocated array of items; caller must free via hu_cross_channel_items_free.
 * Gracefully handles missing reflection_patterns table (AC-7). */
#ifdef HU_ENABLE_SQLITE
hu_error_t hu_cross_channel_collect(hu_allocator_t *alloc, sqlite3 *db, const char *current_channel,
                                    const char *current_contact_id, int64_t now_ms,
                                    int max_candidates, hu_cross_channel_item_t **out_items,
                                    size_t *out_count);
#endif

/* Format time relative to now (e.g. "2 hours ago", "yesterday at 3pm")
 * Extracted to public so it can be used by both daemon and cross_channel pipeline. */
void hu_cross_channel_format_when(char *buf, size_t buflen, int64_t observed_ms, int64_t now_ms);

/* Format stage: builds human-readable cross-channel context string
 * Formats items with provenance (origin channel + relative time) + text.
 * Allocates output string via alloc; caller must free with alloc->free. */
hu_error_t hu_cross_channel_format(hu_allocator_t *alloc, int64_t now_ms,
                                   const hu_cross_channel_item_t *items, size_t count,
                                   char **out_text, size_t *out_len);

/* Free items array allocated by hu_cross_channel_collect
 * Frees both the text pointers and the items array itself. */
void hu_cross_channel_items_free(hu_allocator_t *alloc, hu_cross_channel_item_t *items,
                                 size_t count);

#endif
