#ifndef HU_CHANNEL_CLASS_H
#define HU_CHANNEL_CLASS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_CHANNEL_CLASS_UNKNOWN = 0,
    HU_CHANNEL_CLASS_TEXT_FAST,  /* iMessage, SMS — speed wins, delays read as dispreference */
    HU_CHANNEL_CLASS_TEXT_ASYNC, /* Slack, Discord, Telegram — pauses tolerated */
    HU_CHANNEL_CLASS_VOICE,      /* Voice channel — 200-500ms filler window */
} hu_channel_class_t;

/* Look up the class for a channel name as returned by vtable->name().
 * Case-insensitive. NULL or unknown name returns HU_CHANNEL_CLASS_UNKNOWN. */
hu_channel_class_t hu_channel_class_for_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HU_CHANNEL_CLASS_H */
