/* src/channels/imessage_chat_kind.c — see include/human/channels/imessage_chat_kind.h. */
#include "human/channels/imessage_chat_kind.h"

bool hu_imessage_chat_is_group(int chat_style, int handle_count) {
    if (chat_style == HU_IMESSAGE_CHAT_STYLE_GROUP)
        return true;
    if (chat_style == HU_IMESSAGE_CHAT_STYLE_DIRECT)
        return false;
    /* Style unknown. chat_handle_join excludes the owner, so any chat with 2+
     * other handles has 3+ people. `> 2` here would call a 3-person group a DM. */
    return handle_count >= 2;
}
