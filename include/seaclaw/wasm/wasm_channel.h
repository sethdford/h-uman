#ifndef SC_WASM_CHANNEL_H
#define SC_WASM_CHANNEL_H

#ifdef __wasi__

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

sc_error_t sc_wasm_channel_create(sc_allocator_t *alloc, sc_channel_t *out);
void sc_wasm_channel_destroy(sc_channel_t *ch);

/* Read a line from stdin via WASI. Caller must free. Returns NULL on EOF. */
char *sc_wasm_channel_readline(sc_allocator_t *alloc, size_t *out_len);

#endif /* __wasi__ */

#endif /* SC_WASM_CHANNEL_H */
