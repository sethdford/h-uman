#ifndef HU_MAX_TOKENS_H
#define HU_MAX_TOKENS_H

#include "human/config_types.h"
#include <stddef.h>
#include <stdint.h>

uint32_t hu_max_tokens_default(void);
uint32_t hu_max_tokens_lookup(const char *model_ref, size_t len);
uint32_t hu_max_tokens_resolve(uint32_t override, const char *model_ref, size_t len);

#endif /* HU_MAX_TOKENS_H */
