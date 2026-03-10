#ifndef HU_CHANNELS_DISPATCH_H
#define HU_CHANNELS_DISPATCH_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

hu_error_t hu_dispatch_create(hu_allocator_t *alloc, hu_channel_t *out);
void hu_dispatch_destroy(hu_channel_t *ch);

/** Add a sub-channel to route send() to. Sub must outlive the dispatch channel. */
hu_error_t hu_dispatch_add_channel(hu_channel_t *dispatch_ch, const hu_channel_t *sub);

#endif /* HU_CHANNELS_DISPATCH_H */
