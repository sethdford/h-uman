#ifndef HU_CHANNELS_PWA_H
#define HU_CHANNELS_PWA_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

/* Create a PWA channel that monitors browser tabs for new messages.
 * apps: NULL-terminated array of app names to monitor (e.g. {"slack", "discord", NULL})
 * If apps is NULL, monitors all known PWA drivers with read_messages_js. */
hu_error_t hu_pwa_channel_create(hu_allocator_t *alloc, const char *const *apps,
                                 size_t app_count, hu_channel_t *out);
void hu_pwa_channel_destroy(hu_channel_t *ch);

/* Poll all monitored PWA tabs for new messages. */
hu_error_t hu_pwa_channel_poll(void *channel_ctx, hu_allocator_t *alloc,
                               hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count);

#if HU_IS_TEST
hu_error_t hu_pwa_channel_test_inject(hu_channel_t *ch, const char *app, const char *content);
const char *hu_pwa_channel_test_get_last(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_PWA_H */
