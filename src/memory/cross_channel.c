#include "human/memory/cross_channel.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* --- Pure security predicate: ACL check ---
 *
 * This is a testable, allocation-free decision point that can be
 * unit-tested in isolation (no DB, no persona malloc involved).
 * Both the production code and the tests call the same predicate.
 *
 * Contract:
 * - persona: the agent's persona (contains cross_channel_acl rules)
 * - origin_relationship_type: the source relationship (e.g. "family", "coworker")
 * - turn_relationship_type: the target context relationship (e.g. "family", NULL)
 *
 * Returns HU_XCHAN_ACL_ALLOW or HU_XCHAN_ACL_DENY.
 *
 * Deny cases:
 * - persona is NULL → deny (no ACL rules)
 * - origin is NULL → deny (unknown source)
 * - turn is NULL → deny (unknown target, fail closed)
 * - origin → turn not in allowed list → deny
 */
hu_xchan_acl_decision_t hu_cross_channel_acl_check(const hu_persona_t *persona,
                                                   const char *origin_relationship_type,
                                                   const char *turn_relationship_type) {
    /* Null checks: fail closed. NULL relationships deny regardless of default_policy.
     * This is a deliberate fail-closed choice: unknown source/target → deny. */
    if (!persona || !origin_relationship_type || !turn_relationship_type)
        return HU_XCHAN_ACL_DENY;

    const hu_xchan_acl_t *acl = &persona->cross_channel_acl;

    /* Find the rule for the origin relationship type */
    for (size_t i = 0; i < acl->rule_count; i++) {
        if (strcmp(acl->rules[i].relationship_type, origin_relationship_type) == 0) {
            /* Rule found. Check if turn is in the allow_list */
            for (size_t j = 0; j < acl->rules[i].allow_count; j++) {
                if (strcmp(acl->rules[i].allow_list[j], turn_relationship_type) == 0)
                    return HU_XCHAN_ACL_ALLOW;
            }
            /* Origin found but turn not in allow_list → deny */
            return HU_XCHAN_ACL_DENY;
        }
    }

    /* No rule for this origin relationship type → consult default_policy
     * (operators can override fail-closed behavior via AC-3/AC-4 config).
     * But default is deny_unknown for safety. */
    if (acl->default_policy[0] != '\0' && strcmp(acl->default_policy, "allow_unknown") == 0)
        return HU_XCHAN_ACL_ALLOW;
    return HU_XCHAN_ACL_DENY;
}

/* --- Filter stage: in-place compaction of items array ---
 *
 * Removes items from the array by compacting (skipping disallowed items).
 * Returns count as the new length of the items array.
 *
 * Pseudocode:
 * for each item in items:
 *   if acl_check(item.origin_relationship_type, turn_relationship_type, acl) == ALLOW:
 *     keep it
 *   else:
 *     skip it (mark for removal)
 * compact: shift all kept items to fill the gaps, decrement count
 */
hu_error_t hu_cross_channel_filter(const hu_persona_t *persona, const char *turn_relationship_type,
                                   hu_cross_channel_item_t *items, size_t *inout_count) {
    if (!persona || !items || !inout_count)
        return HU_ERR_INVALID_ARGUMENT;

    /* Walk the array, keeping allowed items and shifting them to compact */
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < *inout_count; read_idx++) {
        const hu_cross_channel_item_t *item = &items[read_idx];
        hu_xchan_acl_decision_t decision = hu_cross_channel_acl_check(
            persona, item->origin_relationship_type, turn_relationship_type);
        if (decision == HU_XCHAN_ACL_ALLOW) {
            /* Keep this item: copy to write position and advance */
            if (write_idx != read_idx) {
                items[write_idx] = items[read_idx];
            }
            write_idx++;
        }
        /* If decision == HU_XCHAN_ACL_DENY, skip (don't copy, don't advance write) */
    }

    /* Update count to the number of kept items */
    *inout_count = write_idx;
    return HU_OK;
}

/* --- Collect stage: read facts + reflection patterns from DB ---
 *
 * Queries personal_model facts where origin_channel != current_channel,
 * and reflection_patterns where channels include multi-channel patterns.
 * Returns allocated array of items with origin_relationship_type filled from
 * contact graph lookup.
 *
 * Gracefully handles missing reflection_patterns table (AC-7).
 */
#ifdef HU_ENABLE_SQLITE
hu_error_t hu_cross_channel_collect(hu_allocator_t *alloc, sqlite3 *db, const char *current_channel,
                                    const char *current_contact_id, int64_t now_ms,
                                    int max_candidates, hu_cross_channel_item_t **out_items,
                                    size_t *out_count) {
    if (!alloc || !db || !out_items || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out_items = NULL;
    *out_count = 0;

    /* now_ms is reserved for reflection-pattern recency weighting (Scope C);
     * the Phase-1 collect stage doesn't consume it yet. */
    (void)now_ms;

    if (!current_channel || !current_channel[0])
        return HU_OK; /* No current channel specified; return empty */

    /* Allocate initial buffer for items */
    size_t cap = (size_t)max_candidates > 0 ? (size_t)max_candidates : 20;
    hu_cross_channel_item_t *items =
        (hu_cross_channel_item_t *)alloc->alloc(alloc->ctx, cap * sizeof(*items));
    if (!items)
        return HU_ERR_OUT_OF_MEMORY;

    size_t count = 0;

    /* Query personal_model facts where origin_channel != current_channel.
     * For now, we use a simplified approach: facts don't have origin_channel
     * in the personal_model table, so we would need to infer it from context.
     * For Task 5 implementation, we'll read from facts that have been
     * extracted and populate origin_channel as "unknown" or derive from
     * contact_handle if available. This is a graceful degradation (AC-7).
     * TODO: Update personal_model schema in reflection sprint to include
     * origin_channel field. For now, leave items array empty from facts.
     */

    /* Query reflection_patterns table (if it exists) for cross-channel patterns.
     * Check if the table exists first. */
    sqlite3_stmt *probe = NULL;
    sqlite3_prepare_v2(
        db, "SELECT name FROM sqlite_master WHERE type='table' AND name='reflection_patterns'", -1,
        &probe, NULL);
    bool has_reflection = (probe && sqlite3_step(probe) == SQLITE_ROW);
    if (probe)
        sqlite3_finalize(probe);

    if (has_reflection) {
        /* Query reflection_patterns for patterns where:
         * - channels_json contains multiple entries (cross-channel pattern), OR
         * - origin_channel is present and != current_channel
         * For now, gracefully degrade: return empty if schema doesn't match. */
        sqlite3_stmt *stmt = NULL;
        const char *query = "SELECT id, observation, observed_at_ms, confidence "
                            "FROM reflection_patterns "
                            "ORDER BY confidence DESC, observed_at_ms DESC "
                            "LIMIT ?";
        if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, max_candidates > 0 ? max_candidates : 20);
            while (sqlite3_step(stmt) == SQLITE_ROW && count < cap) {
                const char *id = (const char *)sqlite3_column_text(stmt, 0);
                const char *observation = (const char *)sqlite3_column_text(stmt, 1);
                int64_t obs_ms = sqlite3_column_int64(stmt, 2);
                double conf = sqlite3_column_double(stmt, 3);

                if (!id || !observation)
                    continue;

                /* Allocate text for this item */
                size_t obs_len = strlen(observation);
                char *text_copy = (char *)alloc->alloc(alloc->ctx, obs_len + 1);
                if (!text_copy) {
                    sqlite3_finalize(stmt);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memcpy(text_copy, observation, obs_len);
                text_copy[obs_len] = '\0';

                /* Populate item */
                hu_cross_channel_item_t *item = &items[count];
                memset(item, 0, sizeof(*item));
                item->source_type = HU_XCHAN_REFLECTION_PATTERN;
                snprintf(item->item_id, sizeof(item->item_id), "%s", id);
                item->text = text_copy;
                item->text_len = obs_len;
                snprintf(item->origin_channel, sizeof(item->origin_channel), "reflection");
                if (current_contact_id)
                    snprintf(item->origin_contact_id, sizeof(item->origin_contact_id), "%s",
                             current_contact_id);
                /* origin_relationship_type left empty; will be resolved by filter caller */
                item->observed_at_ms = obs_ms;
                item->confidence = conf;
                count++;
            }
            sqlite3_finalize(stmt);
        }
    }

    *out_items = items;
    *out_count = count;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */

/* --- Format when: relative time like "2 hours ago" or "yesterday" ---
 *
 * Converts milliseconds since epoch to human-readable relative time.
 * Examples: "just now", "5m ago", "2h ago", "3d ago", "May 27"
 */
void hu_cross_channel_format_when(char *buf, size_t buflen, int64_t observed_ms, int64_t now_ms) {
    if (!buf || buflen == 0)
        return;

    buf[0] = '\0';

    if (observed_ms <= 0) {
        if (buflen >= 7)
            snprintf(buf, buflen, "recent");
        return;
    }

    /* Convert milliseconds to seconds */
    time_t obs_time = (time_t)(observed_ms / 1000LL);
    time_t now_time = (time_t)(now_ms / 1000LL);

    if (obs_time < 0 || now_time < 0) {
        if (buflen >= 7)
            snprintf(buf, buflen, "recent");
        return;
    }

    double diff = (double)(now_time - obs_time);
    if (diff < 0.0) {
        if (buflen >= 7)
            snprintf(buf, buflen, "future");
        return;
    }

    if (diff < 60.0) {
        snprintf(buf, buflen, "just now");
    } else if (diff < 3600.0) {
        snprintf(buf, buflen, "%dm ago", (int)(diff / 60.0));
    } else if (diff < 86400.0) {
        snprintf(buf, buflen, "%dh ago", (int)(diff / 3600.0));
    } else if (diff < 86400.0 * 7.0) {
        snprintf(buf, buflen, "%dd ago", (int)(diff / 86400.0));
    } else {
        /* Format as date: "May 27" or similar */
        struct tm tm_buf;
        localtime_r(&obs_time, &tm_buf);
        strftime(buf, buflen, "%b %d", &tm_buf);
    }
}

/* --- Format stage: convert items to human-readable context string ---
 *
 * Builds a multi-line string like:
 *   From Telegram 2h ago: ...fact text...
 *   From iMessage yesterday: ...pattern text...
 */
hu_error_t hu_cross_channel_format(hu_allocator_t *alloc, int64_t now_ms,
                                   const hu_cross_channel_item_t *items, size_t count,
                                   char **out_text, size_t *out_len) {
    if (!alloc || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out_text = NULL;
    *out_len = 0;

    if (count == 0)
        return HU_OK; /* Empty context is valid */

    /* Estimate total buffer size needed */
    size_t total_len = 0;
    for (size_t i = 0; i < count; i++) {
        /* "From <channel> <when>: <text>\n" ≈ 80 + text_len */
        total_len += 80 + items[i].text_len;
    }
    total_len += 10; /* margin */

    char *buf = (char *)alloc->alloc(alloc->ctx, total_len);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        const hu_cross_channel_item_t *item = &items[i];

        /* Format relative time */
        char when_buf[64];
        hu_cross_channel_format_when(when_buf, sizeof(when_buf), item->observed_at_ms, now_ms);

        /* Append "From <channel> <when>: <text>\n" */
        const char *ch = item->origin_channel[0] ? item->origin_channel : "memory";
        int written = snprintf(buf + pos, total_len - pos, "From %s %s: %.*s\n", ch, when_buf,
                               (int)item->text_len, item->text);

        if (written < 0 || (size_t)written >= total_len - pos) {
            /* Buffer overflow; stop here */
            buf[pos] = '\0';
            break;
        }
        pos += (size_t)written;
    }

    buf[pos] = '\0';
    *out_text = buf;
    *out_len = pos;
    return HU_OK;
}

/* --- Free items array allocated by hu_cross_channel_collect ---
 *
 * Frees all text pointers and the items array itself.
 */
void hu_cross_channel_items_free(hu_allocator_t *alloc, hu_cross_channel_item_t *items,
                                 size_t count) {
    if (!alloc || !items)
        return;

    for (size_t i = 0; i < count; i++) {
        if (items[i].text) {
            alloc->free(alloc->ctx, items[i].text, items[i].text_len);
        }
    }
    alloc->free(alloc->ctx, items, count * sizeof(*items));
}
