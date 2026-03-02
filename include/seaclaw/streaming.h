#ifndef SC_STREAMING_H
#define SC_STREAMING_H

#include "seaclaw/provider.h"
#include <stdint.h>

/* Create a text delta chunk (caller keeps ownership of delta string) */
static inline sc_stream_chunk_t sc_stream_chunk_text_delta(const char *delta, size_t delta_len) {
    sc_stream_chunk_t c = {0};
    c.delta = delta;
    c.delta_len = delta_len;
    c.is_final = false;
    c.token_count = delta_len > 0 ? (uint32_t)((delta_len + 3) / 4) : 0;
    return c;
}

/* Create final chunk */
static inline sc_stream_chunk_t sc_stream_chunk_final(void) {
    sc_stream_chunk_t c = {0};
    c.delta = "";
    c.delta_len = 0;
    c.is_final = true;
    c.token_count = 0;
    return c;
}

/* Forward provider chunk to callback if non-skip */
static inline void sc_stream_forward(sc_stream_callback_t callback, void *ctx,
    const sc_stream_chunk_t *chunk)
{
    if (!callback || !chunk) return;
    if (chunk->is_final) {
        callback(ctx, chunk);
        return;
    }
    if (chunk->delta && chunk->delta_len > 0)
        callback(ctx, chunk);
}

#endif /* SC_STREAMING_H */
