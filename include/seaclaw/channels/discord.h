#ifndef SC_CHANNELS_DISCORD_H
#define SC_CHANNELS_DISCORD_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_discord_create(sc_allocator_t *alloc,
    const char *token, size_t token_len,
    sc_channel_t *out);

sc_error_t sc_discord_create_ex(sc_allocator_t *alloc,
    const char *token, size_t token_len,
    const char *const *channel_ids, size_t channel_ids_count,
    const char *bot_id, size_t bot_id_len,
    sc_channel_t *out);

sc_error_t sc_discord_poll(void *channel_ctx,
    sc_allocator_t *alloc,
    sc_channel_loop_msg_t *msgs,
    size_t max_msgs,
    size_t *out_count);

void sc_discord_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_DISCORD_H */
