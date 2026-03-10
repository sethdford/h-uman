#ifndef HU_WASM_CHANNEL_H
#define HU_WASM_CHANNEL_H

#ifdef __wasi__

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_wasm_channel_create(hu_allocator_t *alloc, hu_channel_t *out);
void hu_wasm_channel_destroy(hu_channel_t *ch);

/* Read a line from stdin via WASI. Caller must free. Returns NULL on EOF. */
char *hu_wasm_channel_readline(hu_allocator_t *alloc, size_t *out_len);

#endif /* __wasi__ */

#endif /* HU_WASM_CHANNEL_H */
