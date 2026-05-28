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

#endif
