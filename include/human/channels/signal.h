#ifndef HU_CHANNELS_SIGNAL_H
#define HU_CHANNELS_SIGNAL_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Group policy: "open", "allowlist", "disabled" */
#define HU_SIGNAL_GROUP_POLICY_OPEN      "open"
#define HU_SIGNAL_GROUP_POLICY_ALLOWLIST "allowlist"
#define HU_SIGNAL_GROUP_POLICY_DISABLED  "disabled"

#define HU_SIGNAL_GROUP_TARGET_PREFIX "group:"
#define HU_SIGNAL_MAX_MSG             4096

hu_error_t hu_signal_create(hu_allocator_t *alloc, const char *http_url, size_t http_url_len,
                            const char *account, size_t account_len, hu_channel_t *out);

hu_error_t hu_signal_create_ex(hu_allocator_t *alloc, const char *http_url, size_t http_url_len,
                               const char *account, size_t account_len,
                               const char *const *allow_from, size_t allow_from_count,
                               const char *const *group_allow_from, size_t group_allow_from_count,
                               const char *group_policy, size_t group_policy_len,
                               hu_channel_t *out);

hu_error_t hu_signal_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count);

void hu_signal_destroy(hu_channel_t *ch);

#if HU_IS_TEST
hu_error_t hu_signal_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                      size_t session_key_len, const char *content,
                                      size_t content_len);
const char *hu_signal_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_SIGNAL_H */
