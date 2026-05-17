/* M3 frontier adapter stub — see include/human/ml/m3_frontier_adapter.h */

#include "human/ml/m3_frontier_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hu_m3_frontier_adapter {
    hu_allocator_t *alloc;
    uint32_t schema_version;
    /* M3 first-slice observable signal (2026-05-17). The probe counter
     * increments on every `probe_infer` / `noop_infer` call so tests
     * (and operators, eventually) can prove the chat-path call sites
     * actually reach the adapter at runtime. Before this counter the
     * "noop" was undetectable — a regression that dropped one of the
     * 11 `hu_agent_m3_on_provider_success` call sites would still see
     * green. See docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md
     * (Phase B-pre: observability slice). */
    uint64_t probe_count;
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
