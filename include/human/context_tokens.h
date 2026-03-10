#ifndef HU_CONTEXT_TOKENS_H
#define HU_CONTEXT_TOKENS_H

#include "human/config_types.h"
#include <stddef.h>
#include <stdint.h>

uint64_t hu_context_tokens_default(void);
uint64_t hu_context_tokens_lookup(const char *model_ref, size_t len);
uint64_t hu_context_tokens_resolve(uint64_t override, const char *model_ref, size_t len);

#endif /* HU_CONTEXT_TOKENS_H */
