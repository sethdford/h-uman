#ifndef SC_CHANNELS_MAIXCAM_H
#define SC_CHANNELS_MAIXCAM_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdint.h>

sc_error_t sc_maixcam_create(sc_allocator_t *alloc,
    const char *host, size_t host_len,
    uint16_t port,
    sc_channel_t *out);
void sc_maixcam_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_MAIXCAM_H */
