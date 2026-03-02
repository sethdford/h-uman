#ifndef SC_MAX_TOKENS_H
#define SC_MAX_TOKENS_H

#include "seaclaw/config_types.h"
#include <stdint.h>
#include <stddef.h>

uint32_t sc_max_tokens_default(void);
uint32_t sc_max_tokens_lookup(const char *model_ref, size_t len);
uint32_t sc_max_tokens_resolve(uint32_t override, const char *model_ref, size_t len);

#endif /* SC_MAX_TOKENS_H */
