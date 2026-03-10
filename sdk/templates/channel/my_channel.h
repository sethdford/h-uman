#ifndef HU_MY_CHANNEL_H
#define HU_MY_CHANNEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/channel.h"
#include <stddef.h>

/**
 * Create a custom channel implementing hu_channel_t.
 *
 * Register with channel manager:
 *   hu_channel_t ch;
 *   hu_my_channel_create(&alloc, &ch);
 *   hu_channel_manager_register(&mgr, "my_channel", "default", &ch,
 *       HU_CHANNEL_LISTENER_SEND_ONLY);
 */
hu_error_t hu_my_channel_create(hu_allocator_t *alloc, hu_channel_t *out);

void hu_my_channel_destroy(hu_channel_t *ch);

#endif /* HU_MY_CHANNEL_H */
