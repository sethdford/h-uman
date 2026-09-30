/* include/human/channels/imessage_chat_kind.h
 *
 * One answer to "is this chat.db chat a group?", shared by every reader.
 *
 * chat.db records the chat kind in `chat.style`: 43 = group, 45 = 1:1. That
 * column is authoritative. The handle count is NOT a safe proxy on its own:
 * `chat_handle_join` lists the OTHER participants and never the owner, so a
 * 1:1 chat has exactly 1 handle and a 3-person group (owner + 2) has 2. A
 * `count > 2` test therefore called every 3-person group a DM.
 *
 * Pure and platform-independent; compiled unconditionally
 * (src/channels/imessage_chat_kind.c) so the daemon observer, which is built
 * on every platform, can share it. */
#ifndef HU_CHANNELS_IMESSAGE_CHAT_KIND_H
#define HU_CHANNELS_IMESSAGE_CHAT_KIND_H

#include <stdbool.h>

#define HU_IMESSAGE_CHAT_STYLE_UNKNOWN (-1) /* NULL / column absent */
#define HU_IMESSAGE_CHAT_STYLE_GROUP   43
#define HU_IMESSAGE_CHAT_STYLE_DIRECT  45

/* Scalar subqueries over a `message` row aliased `m`, used by the poll SQL in
 * src/channels/imessage.c and by tests against a chat.db fixture. Only valid
 * when the chat table has a `style` column (probe first). */
#define HU_IMESSAGE_SQL_CHAT_STYLE_OF_MESSAGE       \
    "(SELECT cks.style FROM chat_message_join ckj " \
    "  JOIN chat cks ON cks.ROWID = ckj.chat_id "   \
    "  WHERE ckj.message_id = m.ROWID LIMIT 1)"

#define HU_IMESSAGE_SQL_HANDLE_COUNT_OF_MESSAGE                            \
    "COALESCE("                                                            \
    "  (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj " \
    "   JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "         \
    "   WHERE cmj.message_id = m.ROWID), 0)"

/* Pure: whether a chat is a group.
 *   chat_style   — chat.style, or HU_IMESSAGE_CHAT_STYLE_UNKNOWN when NULL or
 *                  the column is absent.
 *   handle_count — COUNT(DISTINCT handle_id) in chat_handle_join for the chat
 *                  (excludes the owner).
 * A known style decides. Otherwise falls back to the handle count. */
bool hu_imessage_chat_is_group(int chat_style, int handle_count);

#endif /* HU_CHANNELS_IMESSAGE_CHAT_KIND_H */
