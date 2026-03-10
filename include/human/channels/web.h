#ifndef HU_CHANNELS_WEB_H
#define HU_CHANNELS_WEB_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

hu_error_t hu_web_create(hu_allocator_t *alloc, hu_channel_t *out);

hu_error_t hu_web_create_with_token(hu_allocator_t *alloc, const char *auth_token,
                                    size_t auth_token_len, hu_channel_t *out);

bool hu_web_validate_token(const hu_channel_t *ch, const char *candidate, size_t candidate_len);

void hu_web_destroy(hu_channel_t *ch);

#if HU_IS_TEST
hu_error_t hu_web_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                   size_t session_key_len, const char *content, size_t content_len);
const char *hu_web_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_WEB_H */
