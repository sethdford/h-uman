#include "human/channels/behavior_class.h"
#include <strings.h> /* strncasecmp */

struct class_entry {
    const char *name;
    int klass;
};

/* Canonical channel names -> behavior class. Mirrors the legacy table that
 * lived in agent_turn.c::at_behavior_channel_class; this is now the single
 * source of truth. Add a row here when you add a channel — agent core never
 * changes. */
static const struct class_entry k_table[] = {
    {"voice", HU_CHANNEL_BEHAVIOR_VOICE},    {"email", HU_CHANNEL_BEHAVIOR_EMAIL},
    {"imap", HU_CHANNEL_BEHAVIOR_EMAIL},     {"gmail", HU_CHANNEL_BEHAVIOR_EMAIL},
    {"telegram", HU_CHANNEL_BEHAVIOR_CHAT},  {"discord", HU_CHANNEL_BEHAVIOR_CHAT},
    {"slack", HU_CHANNEL_BEHAVIOR_CHAT},     {"mattermost", HU_CHANNEL_BEHAVIOR_CHAT},
    {"matrix", HU_CHANNEL_BEHAVIOR_CHAT},    {"irc", HU_CHANNEL_BEHAVIOR_CHAT},
    {"line", HU_CHANNEL_BEHAVIOR_CHAT},      {"lark", HU_CHANNEL_BEHAVIOR_CHAT},
    {"messenger", HU_CHANNEL_BEHAVIOR_CHAT}, {"whatsapp", HU_CHANNEL_BEHAVIOR_CHAT},
    {"imessage", HU_CHANNEL_BEHAVIOR_CHAT},  {"sms", HU_CHANNEL_BEHAVIOR_CHAT},
};

int hu_channel_behavior_class_for_name(const char *name, size_t name_len) {
    if (!name || name_len == 0) {
        return HU_CHANNEL_BEHAVIOR_DEFAULT;
    }
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); i++) {
        const char *cand = k_table[i].name;
        size_t cand_len = 0;
        while (cand[cand_len] != '\0') {
            cand_len++;
        }
        /* Exact, case-insensitive match. Length equality prevents the legacy
         * prefix-collision bug where memcmp(cn, "imessage", 8) also matched
         * "imessagebot". */
        if (cand_len == name_len && strncasecmp(name, cand, name_len) == 0) {
            return k_table[i].klass;
        }
    }
    return HU_CHANNEL_BEHAVIOR_DEFAULT;
}
