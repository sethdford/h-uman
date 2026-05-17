/* M3 frontier adapter stub — see include/human/ml/m3_frontier_adapter.h */

#include "human/ml/m3_frontier_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pin the on-wire outcome record size so a future field addition can't
 * silently bloat the ring buffer beyond the agreed 384 KB ceiling
 * (4096 × 96 bytes). If this fires, either the struct grew or the
 * #define drifted — both need explicit attention, not a paper-over. */
_Static_assert(sizeof(hu_m3_inference_outcome_t) == HU_M3_OUTCOME_RECORD_BYTES,
               "hu_m3_inference_outcome_t must be exactly HU_M3_OUTCOME_RECORD_BYTES");

struct hu_m3_frontier_adapter {
    hu_allocator_t *alloc;
    uint32_t schema_version;
    /* M3 first-slice observable signal (2026-05-17). The probe counter
     * increments on every `probe_infer` / `noop_infer` call so tests
     * (and operators, eventually) can prove the chat-path call sites
     * actually reach the adapter at runtime. */
    uint64_t probe_count;

    /* B1 (redefined 2026-05-17): inference outcome ring buffer. Fixed
     * capacity so the adapter's memory footprint is bounded at open.
     * `head` is the index where the NEXT outcome will be written;
     * writes always succeed and overwrite the oldest slot when full.
     * `total_recorded` is monotonic — every outcome ever written
     * counts, even those overwritten. The snapshot reader uses
     * `total_recorded < CAPACITY` to know whether to start from 0 or
     * from `head` (the oldest live slot in a full buffer). */
    hu_m3_inference_outcome_t outcomes[HU_M3_OUTCOMES_RING_CAPACITY];
    size_t head;
    uint64_t total_recorded;
};

bool hu_m3_adapter_should_disable(bool cfg_disabled) {
    if (cfg_disabled)
        return true;
    /* Env override: any non-empty value other than "0" disables. The
     * "0" exception lets operators explicitly re-enable in scripts
     * that always set the env var (mirrors the convention used for
     * other HUMAN_* boolean envs in the codebase). */
    const char *env = getenv("HUMAN_M3_ADAPTER_DISABLE");
    if (!env || env[0] == '\0')
        return false;
    if (env[0] == '0' && env[1] == '\0')
        return false;
    return true;
}

/* Track D D2.1 — honest-gap caveat strings.
 *
 * Single source of truth for the user-facing disclaimer printed by
 * `human ml lora-persona`. The block is intentionally unambiguous —
 * tests pin specific substrings (the "NOT" disclaimer, the model name,
 * the doc-path) so a refactor that softens the language fails CI. */

#define HU_ML_LORA_PERSONA_CAVEAT_DOC_PATH "docs/plans/2026-05-10-m3-frontier-model-bridge.md"

const char *hu_ml_lora_persona_caveat_doc_path(void) {
    return HU_ML_LORA_PERSONA_CAVEAT_DOC_PATH;
}

const char *hu_ml_lora_persona_caveat_block(void) {
    return "[lora-persona] NOTE: trains LoRA on the reference HUML GPT.\n"
           "[lora-persona]       This is not a frontier model fine-tune.\n"
           "[lora-persona]       For Llama/Qwen/Mistral fine-tuning, see\n"
           "[lora-persona]       " HU_ML_LORA_PERSONA_CAVEAT_DOC_PATH "\n";
}

hu_error_t hu_m3_frontier_adapter_try_open(hu_allocator_t *alloc, const char *path, size_t path_len,
                                           hu_m3_frontier_adapter_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_IO;

    unsigned char hdr[16];
    size_t n = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    if (n < 8)
        return HU_ERR_IO;
    if (memcmp(hdr, HU_M3_ADAPTER_MAGIC, 8) != 0)
        return HU_ERR_IO;

    uint32_t ver = 0;
    if (n >= 12) {
        ver = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
              ((uint32_t)hdr[11] << 24);
    }
    if (ver == 0)
        ver = 1;

    hu_m3_frontier_adapter_t *a = (hu_m3_frontier_adapter_t *)alloc->alloc(alloc->ctx, sizeof(*a));
    if (!a)
        return HU_ERR_OUT_OF_MEMORY;
    memset(a, 0, sizeof(*a));
    a->alloc = alloc;
    a->schema_version = ver;
    *out = a;
    return HU_OK;
}

hu_error_t hu_m3_frontier_adapter_probe_infer(hu_m3_frontier_adapter_t *adapter) {
    /* First-slice (2026-05-17) bridge from the silent (void)return to a
     * real, observable signal. No tensors yet — see
     * docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md for the
     * phased path to actual MLX/llama.cpp inference. The point of the
     * counter is that a regression which drops a chat-path call site
     * stops being undetectable: the probe count fails to advance and
     * `test_m3_frontier_probe.c` goes red. */
    if (!adapter)
        return HU_OK;
    adapter->probe_count++;
    return HU_OK;
}

hu_error_t hu_m3_frontier_adapter_noop_infer(hu_m3_frontier_adapter_t *adapter) {
    /* Backwards-compat wrapper. Existing 11 call sites in agent_turn.c
     * + agent_stream.c (via hu_agent_m3_on_provider_success) and the
     * daemon's M3 probe-path bootstrap (`src/daemon.c`) still call
     * `noop_infer` — they all reach `probe_infer` so the counter
     * advances. Renaming the public symbol is a follow-up
     * (Phase B-pre.2 in the execution plan). */
    return hu_m3_frontier_adapter_probe_infer(adapter);
}

uint64_t hu_m3_frontier_adapter_probe_count(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->probe_count : 0;
}

void hu_m3_frontier_adapter_close(hu_allocator_t *alloc, hu_m3_frontier_adapter_t *adapter) {
    if (!alloc || !adapter)
        return;
    alloc->free(alloc->ctx, adapter, sizeof(*adapter));
}

uint32_t hu_m3_frontier_adapter_schema_version(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->schema_version : 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * Phase B1 (redefined 2026-05-17): inference outcome capture
 * ───────────────────────────────────────────────────────────────── */

/* FNV-1a 64-bit. Standard parameters (offset basis + prime from
 * https://tools.ietf.org/html/draft-eastlake-fnv-21). Pure function;
 * deterministic across runs. Returns 0 for NULL/empty input — this is
 * a SENTINEL the outcome struct uses to mean "no value" (e.g. for
 * contact_id_hash when there's no contact context). */
uint64_t hu_m3_outcome_hash_bytes(const void *data, size_t len) {
    if (!data || len == 0)
        return 0;
    const unsigned char *p = (const unsigned char *)data;
    uint64_t hash = 0xcbf29ce484222325ULL; /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x100000001b3ULL; /* FNV prime */
    }
    /* Reserve 0 for "no value" — if FNV happens to land on 0, bump
     * to 1 so callers can distinguish "hash is zero" from "no hash". */
    return hash ? hash : 1ULL;
}

hu_error_t hu_m3_frontier_adapter_record_outcome(hu_m3_frontier_adapter_t *adapter,
                                                 const hu_m3_inference_outcome_t *outcome) {
    /* NULL adapter is a deliberate no-op — agent paths shouldn't have
     * to gate the call, and the M3 system is opt-in via configuration.
     * Returning HU_OK here matches probe_infer's NULL semantics. */
    if (!adapter)
        return HU_OK;
    if (!outcome)
        return HU_ERR_INVALID_ARGUMENT;

    /* Copy into the slot at head; advance head; wrap on capacity. The
     * ring is single-writer (the agent serializes turns) so no atomic
     * is needed today. If concurrent writers ever appear, add a
     * spinlock or migrate head/total_recorded to atomic — the snapshot
     * reader is already designed to tolerate a slightly stale view. */
    adapter->outcomes[adapter->head] = *outcome;
    adapter->head = (adapter->head + 1u) % HU_M3_OUTCOMES_RING_CAPACITY;
    adapter->total_recorded++;
    return HU_OK;
}

uint64_t hu_m3_frontier_adapter_outcomes_recorded(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->total_recorded : 0ULL;
}

hu_error_t hu_m3_frontier_adapter_snapshot_outcomes(const hu_m3_frontier_adapter_t *adapter,
                                                    hu_m3_inference_outcome_t *out_buf,
                                                    size_t max_count, size_t *out_count) {
    if (!out_buf || !out_count || max_count == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (!adapter)
        return HU_OK;

    /* Two layout cases:
     *   1. total_recorded < CAPACITY: ring not yet wrapped. The live
     *      outcomes are outcomes[0 .. head-1] in storage order.
     *   2. total_recorded >= CAPACITY: ring has wrapped. The oldest
     *      live outcome is at index `head`; the newest is at
     *      `(head - 1 + CAPACITY) % CAPACITY`. Live count = CAPACITY.
     *
     * The output ordering contract is "oldest-of-the-snapshot first".
     * We compute the start index and how many records to copy, then
     * copy in two segments if the snapshot wraps. */
    size_t live_count;
    size_t start;
    if (adapter->total_recorded < (uint64_t)HU_M3_OUTCOMES_RING_CAPACITY) {
        live_count = (size_t)adapter->total_recorded;
        start = 0;
    } else {
        live_count = (size_t)HU_M3_OUTCOMES_RING_CAPACITY;
        start = adapter->head;
    }

    size_t to_copy = live_count < max_count ? live_count : max_count;
    /* When max_count < live_count, return the most-recent `max_count`
     * outcomes — slide the start forward by (live_count - to_copy).
     * Otherwise start as computed above. */
    if (to_copy < live_count) {
        size_t skip = live_count - to_copy;
        start = (start + skip) % HU_M3_OUTCOMES_RING_CAPACITY;
    }

    /* Copy in up to two segments (may wrap around the end of the ring). */
    size_t first_segment = HU_M3_OUTCOMES_RING_CAPACITY - start;
    if (first_segment > to_copy)
        first_segment = to_copy;
    memcpy(out_buf, &adapter->outcomes[start], first_segment * sizeof(*out_buf));
    if (to_copy > first_segment) {
        memcpy(out_buf + first_segment, &adapter->outcomes[0],
               (to_copy - first_segment) * sizeof(*out_buf));
    }

    *out_count = to_copy;
    return HU_OK;
}
