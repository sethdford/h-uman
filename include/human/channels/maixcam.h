#ifndef HU_CHANNELS_MAIXCAM_H
#define HU_CHANNELS_MAIXCAM_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_maixcam_create(hu_allocator_t *alloc, const char *host, size_t host_len,
                             uint16_t port, hu_channel_t *out);
void hu_maixcam_destroy(hu_channel_t *ch);

#if HU_IS_TEST
hu_error_t hu_maixcam_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                       size_t session_key_len, const char *content,
                                       size_t content_len);
const char *hu_maixcam_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_MAIXCAM_H */
