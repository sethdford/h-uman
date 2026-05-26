/* src/agent/outbound/stats.c
 *
 * Sprint 60 — outbound pipeline stats. Per-stage × per-verdict
 * atomic counters readable via snapshot. See header for contract.
 *
 * Storage: HU_OUTBOUND_STATS_STAGE_COUNT × 4 = 28 cells, each an
 * atomic_uint_least64_t. Total 224 bytes static — sub-cache-line
 * cost. Relaxed atomic ordering: counts are monotonic, operators
 * read approximate values, no per-cell ordering invariant.
 *
 * Stage-name mapping is a small static table; case-sensitive
 * because pipeline_configs.c uses lowercase names exclusively. */

#include "human/agent/outbound_stats.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

/* Atomic counter table. */
static atomic_uint_least64_t
    s_counts[HU_OUTBOUND_STATS_STAGE_COUNT][HU_OUTBOUND_STATS_VERDICT_COUNT];

/* Stage name → enum mapping. Stable order; matches the enum in the
 * header (which doctor JSON output relies on). Adding a new stage
 * MUST append to this table AND to the enum simultaneously. */
static const struct {
    const char *name;
    hu_outbound_stats_stage_t id;
} s_stage_table[] = {
    {"strip", HU_OUTBOUND_STATS_STAGE_STRIP},
    {"shape", HU_OUTBOUND_STATS_STAGE_SHAPE},
    {"echo", HU_OUTBOUND_STATS_STAGE_ECHO},
    {"crosstalk", HU_OUTBOUND_STATS_STAGE_CROSSTALK},
    {"persona", HU_OUTBOUND_STATS_STAGE_PERSONA},
    {"moderation", HU_OUTBOUND_STATS_STAGE_MODERATION},
};

hu_outbound_stats_stage_t hu_outbound_stats_stage_from_name(const char *stage_name) {
    if (!stage_name || stage_name[0] == '\0')
        return HU_OUTBOUND_STATS_STAGE_OTHER;
    size_t n = sizeof(s_stage_table) / sizeof(s_stage_table[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(stage_name, s_stage_table[i].name) == 0)
            return s_stage_table[i].id;
    }
    return HU_OUTBOUND_STATS_STAGE_OTHER;
}

const char *hu_outbound_stats_stage_name(hu_outbound_stats_stage_t stage) {
    switch (stage) {
    case HU_OUTBOUND_STATS_STAGE_STRIP:
        return "strip";
    case HU_OUTBOUND_STATS_STAGE_SHAPE:
        return "shape";
    case HU_OUTBOUND_STATS_STAGE_ECHO:
        return "echo";
    case HU_OUTBOUND_STATS_STAGE_CROSSTALK:
        return "crosstalk";
    case HU_OUTBOUND_STATS_STAGE_PERSONA:
        return "persona";
    case HU_OUTBOUND_STATS_STAGE_MODERATION:
        return "moderation";
    case HU_OUTBOUND_STATS_STAGE_OTHER:
        return "other";
    case HU_OUTBOUND_STATS_STAGE_COUNT:
    default:
        return "?";
    }
}

void hu_outbound_stats_record(const char *stage_name, int verdict_kind) {
    /* Defensive: clamp out-of-range verdict_kind to OUT (drop into
     * the OTHER bucket's 0th slot would be a silent corruption;
     * better to silently drop the count than to write past the
     * verdict-array bound). */
    if (verdict_kind < 0 || verdict_kind >= HU_OUTBOUND_STATS_VERDICT_COUNT)
        return;
    hu_outbound_stats_stage_t stage = hu_outbound_stats_stage_from_name(stage_name);
    if (stage < 0 || stage >= HU_OUTBOUND_STATS_STAGE_COUNT)
        return;
    atomic_fetch_add_explicit(&s_counts[stage][verdict_kind], 1u, memory_order_relaxed);
}

hu_error_t hu_outbound_stats_snapshot(hu_outbound_stats_snapshot_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t s = 0; s < HU_OUTBOUND_STATS_STAGE_COUNT; s++) {
        for (size_t v = 0; v < HU_OUTBOUND_STATS_VERDICT_COUNT; v++) {
            out->counts[s][v] =
                (uint64_t)atomic_load_explicit(&s_counts[s][v], memory_order_relaxed);
        }
    }
    return HU_OK;
}

void hu_outbound_stats_reset_for_test(void) {
    for (size_t s = 0; s < HU_OUTBOUND_STATS_STAGE_COUNT; s++) {
        for (size_t v = 0; v < HU_OUTBOUND_STATS_VERDICT_COUNT; v++) {
            atomic_store_explicit(&s_counts[s][v], 0u, memory_order_relaxed);
        }
    }
}
