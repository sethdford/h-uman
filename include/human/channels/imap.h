#ifndef HU_CHANNELS_IMAP_H
#define HU_CHANNELS_IMAP_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_imap_config {
    const char *imap_host;
    size_t imap_host_len;
    uint16_t imap_port;
    const char *imap_username;
    size_t imap_username_len;
    const char *imap_password;
    size_t imap_password_len;
    const char *imap_folder;
    size_t imap_folder_len;
    bool imap_use_tls;
} hu_imap_config_t;

hu_error_t hu_imap_create(hu_allocator_t *alloc, const hu_imap_config_t *config, hu_channel_t *out);
void hu_imap_destroy(hu_channel_t *ch);
bool hu_imap_is_configured(hu_channel_t *ch);

/** Poll IMAP inbox for new messages. Under HU_IS_TEST returns mock emails from queue. */
hu_error_t hu_imap_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count);

#if HU_IS_TEST
/** Push a mock email to the queue for poll to return. */
hu_error_t hu_imap_test_push_mock(hu_channel_t *ch, const char *session_key, size_t session_key_len,
                                  const char *content, size_t content_len);
#endif

#endif /* HU_CHANNELS_IMAP_H */
