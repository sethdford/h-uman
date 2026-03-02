#ifndef SC_CHANNELS_TELEGRAM_H
#define SC_CHANNELS_TELEGRAM_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdbool.h>

sc_error_t sc_telegram_create(sc_allocator_t *alloc,
    const char *token, size_t token_len,
    sc_channel_t *out);

sc_error_t sc_telegram_create_ex(sc_allocator_t *alloc,
    const char *token, size_t token_len,
    const char *const *allow_from, size_t allow_from_count,
    sc_channel_t *out);

void sc_telegram_set_allowlist(sc_channel_t *ch,
    const char *const *allow_from, size_t allow_from_count);

const char *sc_telegram_commands_help(void);

char *sc_telegram_escape_markdown_v2(sc_allocator_t *alloc,
    const char *text, size_t len, size_t *out_len);

sc_error_t sc_telegram_poll(void *channel_ctx,
    sc_allocator_t *alloc,
    sc_channel_loop_msg_t *msgs,
    size_t max_msgs,
    size_t *out_count);

void sc_telegram_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_TELEGRAM_H */
