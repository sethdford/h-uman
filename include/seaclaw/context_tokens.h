#ifndef SC_CONTEXT_TOKENS_H
#define SC_CONTEXT_TOKENS_H

#include "seaclaw/config_types.h"
#include <stdint.h>
#include <stddef.h>

uint64_t sc_context_tokens_default(void);
uint64_t sc_context_tokens_lookup(const char *model_ref, size_t len);
uint64_t sc_context_tokens_resolve(uint64_t override, const char *model_ref, size_t len);

#endif /* SC_CONTEXT_TOKENS_H */
