/* Returns per-provider default stop sequences. */
#ifndef HU_AGENT_STOP_SEQUENCE_REGISTRY_H
#define HU_AGENT_STOP_SEQUENCE_REGISTRY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lookup stop sequences for a provider. Channel parameter is reserved
 * for future per-channel overrides; pass NULL/0 for now.
 *
 * Output:
 *   *out_seqs       — array of NUL-terminated strings; static storage;
 *                     never freed by caller.
 *   *out_seqs_count — number of entries (0 if no defaults known).
 *
 * Returns HU_OK always; missing entries return count == 0. */
hu_error_t hu_stop_sequence_registry_lookup(const char *provider, size_t provider_len,
                                            const char *channel, size_t channel_len,
                                            const char *const **out_seqs, size_t *out_seqs_count);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_STOP_SEQUENCE_REGISTRY_H */
