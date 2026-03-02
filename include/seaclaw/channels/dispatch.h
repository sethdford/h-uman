#ifndef SC_CHANNELS_DISPATCH_H
#define SC_CHANNELS_DISPATCH_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

sc_error_t sc_dispatch_create(sc_allocator_t *alloc, sc_channel_t *out);
void sc_dispatch_destroy(sc_channel_t *ch);

/** Add a sub-channel to route send() to. Sub must outlive the dispatch channel. */
sc_error_t sc_dispatch_add_channel(sc_channel_t *dispatch_ch, const sc_channel_t *sub);

#endif /* SC_CHANNELS_DISPATCH_H */
