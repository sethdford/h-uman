#ifndef HU_CODEX_CLI_H
#define HU_CODEX_CLI_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

hu_error_t hu_codex_cli_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                               const char *base_url, size_t base_url_len, hu_provider_t *out);

#endif /* HU_CODEX_CLI_H */
