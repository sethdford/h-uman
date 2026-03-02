#ifndef SC_CHANNELS_SIGNAL_H
#define SC_CHANNELS_SIGNAL_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Group policy: "open", "allowlist", "disabled" */
#define SC_SIGNAL_GROUP_POLICY_OPEN       "open"
#define SC_SIGNAL_GROUP_POLICY_ALLOWLIST  "allowlist"
#define SC_SIGNAL_GROUP_POLICY_DISABLED   "disabled"

#define SC_SIGNAL_GROUP_TARGET_PREFIX "group:"
#define SC_SIGNAL_MAX_MSG 4096

sc_error_t sc_signal_create(sc_allocator_t *alloc,
    const char *http_url, size_t http_url_len,
    const char *account, size_t account_len,
    sc_channel_t *out);

sc_error_t sc_signal_create_ex(sc_allocator_t *alloc,
    const char *http_url, size_t http_url_len,
    const char *account, size_t account_len,
    const char *const *allow_from, size_t allow_from_count,
    const char *const *group_allow_from, size_t group_allow_from_count,
    const char *group_policy, size_t group_policy_len,
    sc_channel_t *out);

sc_error_t sc_signal_poll(void *channel_ctx,
    sc_allocator_t *alloc,
    sc_channel_loop_msg_t *msgs,
    size_t max_msgs,
    size_t *out_count);

void sc_signal_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_SIGNAL_H */
