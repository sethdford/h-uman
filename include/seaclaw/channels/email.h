#ifndef SC_CHANNELS_EMAIL_H
#define SC_CHANNELS_EMAIL_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

sc_error_t sc_email_create(sc_allocator_t *alloc, const char *smtp_host, size_t smtp_host_len,
                           uint16_t smtp_port, const char *from_address, size_t from_len,
                           sc_channel_t *out);
void sc_email_destroy(sc_channel_t *ch);

bool sc_email_is_configured(sc_channel_t *ch);

/** Set SMTP authentication credentials (user:pass for curl --user). */
sc_error_t sc_email_set_auth(sc_channel_t *ch, const char *user, size_t user_len, const char *pass,
                             size_t pass_len);

/** Configure IMAP for receiving email. */
sc_error_t sc_email_set_imap(sc_channel_t *ch, const char *imap_host, size_t imap_host_len,
                             uint16_t imap_port);

/** Poll IMAP inbox for new unseen messages. */
sc_error_t sc_email_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count);

#if SC_IS_TEST
const char *sc_email_test_last_message(sc_channel_t *ch);

/** Test hook: inject mock email for poll to return. */
sc_error_t sc_email_test_inject_mock_email(sc_channel_t *ch, const char *from, size_t from_len,
                                           const char *subject_or_body, size_t body_len);
#endif

#endif /* SC_CHANNELS_EMAIL_H */
