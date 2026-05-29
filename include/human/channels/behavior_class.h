#ifndef HU_CHANNELS_BEHAVIOR_CLASS_H
#define HU_CHANNELS_BEHAVIOR_CLASS_H

#include <stddef.h>

/* Behavior class drives tone/length/formality defaults in the agent turn
 * loop (maps to hu_behavior_input_t.channel_class in behavior/policy.h).
 * Owned by the Channels bounded context: adding a channel updates the table
 * in src/channels/behavior_class.c, never the agent core.
 * See docs/standards/engineering/bounded-contexts.md. */
typedef enum hu_channel_behavior_class {
    HU_CHANNEL_BEHAVIOR_DEFAULT = 0,
    HU_CHANNEL_BEHAVIOR_VOICE = 1,
    HU_CHANNEL_BEHAVIOR_CHAT = 2, /* IM / group chat */
    HU_CHANNEL_BEHAVIOR_EMAIL = 3,
} hu_channel_behavior_class_t;

/* Returns the behavior class for a canonical channel name (the factory key,
 * e.g. "imessage", "slack"). Exact, case-insensitive match. NULL/empty/unknown
 * -> HU_CHANNEL_BEHAVIOR_DEFAULT (0). Returns int (not the enum) so existing
 * agent-core call sites that store an int keep their type. */
int hu_channel_behavior_class_for_name(const char *name, size_t name_len);

#endif /* HU_CHANNELS_BEHAVIOR_CLASS_H */
