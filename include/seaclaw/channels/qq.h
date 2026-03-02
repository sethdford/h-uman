#ifndef SC_CHANNELS_QQ_H
#define SC_CHANNELS_QQ_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_qq_create(sc_allocator_t *alloc,
    const char *app_id, size_t app_id_len,
    const char *bot_token, size_t bot_token_len,
    bool sandbox,
    sc_channel_t *out);
void sc_qq_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_QQ_H */
