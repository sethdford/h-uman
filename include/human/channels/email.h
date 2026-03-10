#ifndef HU_CHANNELS_EMAIL_H
#define HU_CHANNELS_EMAIL_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_email_create(hu_allocator_t *alloc, const char *smtp_host, size_t smtp_host_len,
                           uint16_t smtp_port, const char *from_address, size_t from_len,
                           hu_channel_t *out);
void hu_email_destroy(hu_channel_t *ch);

bool hu_email_is_configured(hu_channel_t *ch);

/** Set SMTP authentication credentials (user:pass for curl --user). */
hu_error_t hu_email_set_auth(hu_channel_t *ch, const char *user, size_t user_len, const char *pass,
                             size_t pass_len);

/** Configure IMAP for receiving email. */
hu_error_t hu_email_set_imap(hu_channel_t *ch, const char *imap_host, size_t imap_host_len,
                             uint16_t imap_port);

/** Poll IMAP inbox for new unseen messages. */
hu_error_t hu_email_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count);

#if HU_IS_TEST
const char *hu_email_test_last_message(hu_channel_t *ch);

/** Test hook: inject mock email for poll to return. */
hu_error_t hu_email_test_inject_mock_email(hu_channel_t *ch, const char *from, size_t from_len,
                                           const char *subject_or_body, size_t body_len);
#endif

#endif /* HU_CHANNELS_EMAIL_H */
