#ifndef SC_CHANNELS_TEAMS_H
#define SC_CHANNELS_TEAMS_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_teams_create(sc_allocator_t *alloc, const char *app_id, size_t app_id_len,
                           const char *app_secret, size_t app_secret_len, const char *service_url,
                           size_t service_url_len, sc_channel_t *out);

sc_error_t sc_teams_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                               size_t body_len);

sc_error_t sc_teams_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count);

void sc_teams_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_TEAMS_H */
