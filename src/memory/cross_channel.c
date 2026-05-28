#include "human/memory/cross_channel.h"
#include "human/persona.h"
#include <string.h>

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
    /* Null checks: fail closed */
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

    /* No rule for this origin relationship type → deny (unknown, fail closed) */
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
