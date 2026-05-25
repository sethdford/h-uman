#ifndef HUMAN_CHANNELS_IMESSAGE_ACTION_H
#define HUMAN_CHANNELS_IMESSAGE_ACTION_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HU_REPLY_STYLE_FLAT = 0,
    HU_REPLY_STYLE_THREADED = 1,
    HU_REPLY_STYLE_TAPBACK = 2,
    HU_REPLY_STYLE_TAPBACK_PLUS_FLAT = 3,
} hu_reply_style_t;

#endif
