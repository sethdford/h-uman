/* src/ml/init_dpo_bridge.c
 *
 * Bridge from init_outcome resolutions → dpo_pairs single-sided rows.
 *
 * Lifecycle:
 *   daemon init → hu_dpo_collector_create → hu_init_dpo_bridge_set_collector
 *   resolver tick → hu_init_outcome_resolve_pending → bridge record (per
 *                   resolution) → hu_dpo_record_pair → SQLite INSERT
 *   daemon shutdown → hu_dpo_collector_deinit (bridge holds borrow only)
 *
 * See include/human/ml/init_dpo_bridge.h for the contract.
 */

#ifdef HU_ENABLE_ML

#include "human/ml/init_dpo_bridge.h"

#include "human/ml/dpo.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Module-private collector pointer. Borrowed — daemon owns the underlying
 * struct + DB handle. Set/cleared via hu_init_dpo_bridge_set_collector. */
static struct hu_dpo_collector *s_collector = NULL;

void hu_init_dpo_bridge_set_collector(struct hu_dpo_collector *collector) {
    s_collector = collector;
}

struct hu_dpo_collector *hu_init_dpo_bridge_get_collector(void) {
    return s_collector;
}

hu_error_t hu_init_dpo_bridge_record(hu_allocator_t *alloc, hu_init_resolution_t outcome,
                                     const char *draft, const char *target, int64_t resolution_ts) {
    (void)alloc; /* unused — hu_dpo_record_pair holds its own alloc handle */

    if (!draft)
        return HU_ERR_INVALID_ARGUMENT;
    if (outcome != HU_INIT_RESOLUTION_REPLIED && outcome != HU_INIT_RESOLUTION_IGNORED)
        return HU_ERR_INVALID_ARGUMENT;
    if (!s_collector)
        return HU_ERR_NOT_SUPPORTED;

    /* Build the preference pair. Prompt template is intentionally
     * minimal-but-stable: a downstream pairing pass that wants richer
     * context can JOIN on (timestamp, source) to grab the matching
     * init_outcome JSONL line. Keeping the prompt small here also
     * leaves room for chosen/rejected within the 4096-byte field. */
    hu_preference_pair_t pair;
    memset(&pair, 0, sizeof(pair));

    /* prompt: a short identifier that lets readers locate the row's
     * context without parsing free text. Format:
     *   "proactive-proposal: target=<handle> ts=<unix>"
     * Stable, machine-grep-able, and survives schema growth. */
    const char *safe_target = (target && target[0]) ? target : "unknown";
    int pn = snprintf(pair.prompt, sizeof(pair.prompt), "proactive-proposal: target=%s ts=%lld",
                      safe_target, (long long)resolution_ts);
    if (pn < 0)
        return HU_ERR_IO;
    pair.prompt_len = (size_t)pn < sizeof(pair.prompt) ? (size_t)pn : sizeof(pair.prompt) - 1;

    /* Single-sided per outcome. Truncate at field-cap defensively
     * (pending_proposal_t.draft is 1024, chosen/rejected are 4096, so
     * truncation should never bite — defensive belt). */
    size_t draft_len = strlen(draft);
    if (outcome == HU_INIT_RESOLUTION_REPLIED) {
        size_t copy = draft_len < sizeof(pair.chosen) - 1 ? draft_len : sizeof(pair.chosen) - 1;
        memcpy(pair.chosen, draft, copy);
        pair.chosen[copy] = '\0';
        pair.chosen_len = copy;
        pair.rejected[0] = '\0';
        pair.rejected_len = 0;
    } else { /* IGNORED */
        size_t copy = draft_len < sizeof(pair.rejected) - 1 ? draft_len : sizeof(pair.rejected) - 1;
        memcpy(pair.rejected, draft, copy);
        pair.rejected[copy] = '\0';
        pair.rejected_len = copy;
        pair.chosen[0] = '\0';
        pair.chosen_len = 0;
    }

    /* margin=1.0 for both single-sided cases. A future pairing pass can
     * derive a real margin from confidence-at-fire-time and reply
     * latency; until then, 1.0 is "we observed the outcome with full
     * certainty" (no LLM judgment was involved in the resolution). */
    pair.margin = 1.0;
    pair.timestamp = resolution_ts;

    const char *src = HU_INIT_DPO_BRIDGE_SOURCE;
    size_t src_len = strlen(src);
    size_t copy_s = src_len < sizeof(pair.source) - 1 ? src_len : sizeof(pair.source) - 1;
    memcpy(pair.source, src, copy_s);
    pair.source[copy_s] = '\0';
    pair.source_len = copy_s;

    return hu_dpo_record_pair(s_collector, &pair);
}

#endif /* HU_ENABLE_ML */
