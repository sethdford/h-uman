#ifndef HU_CHANNELS_TELEGRAM_H
#define HU_CHANNELS_TELEGRAM_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_telegram_create(hu_allocator_t *alloc, const char *token, size_t token_len,
                              hu_channel_t *out);

hu_error_t hu_telegram_create_ex(hu_allocator_t *alloc, const char *token, size_t token_len,
                                 const char *const *allow_from, size_t allow_from_count,
                                 hu_channel_t *out);

void hu_telegram_set_allowlist(hu_channel_t *ch, const char *const *allow_from,
                               size_t allow_from_count);

const char *hu_telegram_commands_help(void);

char *hu_telegram_escape_markdown_v2(hu_allocator_t *alloc, const char *text, size_t len,
                                     size_t *out_len);

hu_error_t hu_telegram_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count);

void hu_telegram_destroy(hu_channel_t *ch);

#if HU_IS_TEST
hu_error_t hu_telegram_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len);
const char *hu_telegram_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_TELEGRAM_H */
