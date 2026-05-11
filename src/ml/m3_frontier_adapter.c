/* M3 frontier adapter stub — see include/human/ml/m3_frontier_adapter.h */

#include "human/ml/m3_frontier_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hu_m3_frontier_adapter {
    hu_allocator_t *alloc;
    uint32_t schema_version;
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

#define HU_ML_LORA_PERSONA_CAVEAT_DOC_PATH \
    "docs/plans/2026-05-10-m3-frontier-model-bridge.md"

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

    hu_m3_frontier_adapter_t *a =
        (hu_m3_frontier_adapter_t *)alloc->alloc(alloc->ctx, sizeof(*a));
    if (!a)
        return HU_ERR_OUT_OF_MEMORY;
    memset(a, 0, sizeof(*a));
    a->alloc = alloc;
    a->schema_version = ver;
    *out = a;
    return HU_OK;
}

hu_error_t hu_m3_frontier_adapter_noop_infer(hu_m3_frontier_adapter_t *adapter) {
    (void)adapter;
    return HU_OK;
}

void hu_m3_frontier_adapter_close(hu_allocator_t *alloc, hu_m3_frontier_adapter_t *adapter) {
    if (!alloc || !adapter)
        return;
    alloc->free(alloc->ctx, adapter, sizeof(*adapter));
}

uint32_t hu_m3_frontier_adapter_schema_version(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->schema_version : 0;
}
