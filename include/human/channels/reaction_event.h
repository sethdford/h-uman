/* include/human/channels/reaction_event.h */
#ifndef HU_CHANNELS_REACTION_EVENT_H
#define HU_CHANNELS_REACTION_EVENT_H

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_REACTION_UNKNOWN = 0,
    HU_REACTION_LOVE,
    HU_REACTION_LIKE,
    HU_REACTION_DISLIKE,
    HU_REACTION_LAUGH,
    HU_REACTION_EMPHASIZE,
    HU_REACTION_KIND_QUESTION,
    HU_REACTION_KIND_CUSTOM_EMOJI, /* iMessage code 2006 — Apple Sticker / custom emoji */
} hu_reaction_kind_t;

typedef enum {
    HU_REACTION_NEUTRAL = 0,
    HU_REACTION_POSITIVE = 1,
    HU_REACTION_NEGATIVE = -1,
} hu_reaction_polarity_t;

typedef struct {
    const char *channel_id;         /* "imessage", "slack", ... */
    const char *target_thread_id;   /* chat_guid (iMessage) or channel_id (Slack) */
    const char *target_message_ref; /* associated_message_guid OR ts */
    const char *sender_handle;
    hu_reaction_kind_t kind;
    hu_reaction_polarity_t polarity;
    int64_t timestamp_unix;
    int is_removal; /* 0=add, 1=remove */
    /* Phase 2 of docs/plans/2026-05-18-imessage-sota.md: iOS 17+
     * stores the actual emoji glyph for CUSTOM_EMOJI tapbacks in a
     * separate `associated_message_emoji` column. NULL when the
     * channel doesn't expose the glyph (e.g. standard 7-kind taps
     * pre-iOS 17) or when the channel concept doesn't apply
     * (Slack reactji are name-based, mapped through `kind`).
     * Owned by the producer — must be freed by the consumer in the
     * same place sender_handle / target_thread_id / target_message_ref
     * are freed. */
    const char *emoji;
    /* DEF-8 (2026-10-02): what the reaction points AT, resolved by the
     * producer from the same chat.db statement. target_is_ours = 1 when a
     * CONTACT reacted to an is_from_me row — Seth's own typing included, so it
     * is necessary, not sufficient: the handler attributes the row to the
     * daemon only through an outbound_sends match. target_rowid = that row's
     * chat.db ROWID; target_prev_own_rowid = the previous is_from_me ROWID in
     * the same chat (0 = none); target_sent_ms = its send time (0 = unknown). */
    int target_is_ours;
    int64_t target_rowid;
    int64_t target_prev_own_rowid;
    int64_t target_sent_ms;
} hu_reaction_event_t;

/* iMessage tapback codes — AUTHORITY for the full set comes from the
 * actual switch in src/channels/imessage.c:1812-1832 + the positive-set
 * filter in src/channels/imessage.c:1890-1896 + the SQL range at
 * src/channels/imessage.c:1783 (BETWEEN 2000 AND 2006):
 *
 *  2000 = LOVE         (positive)
 *  2001 = LIKE         (positive)
 *  2002 = DISLIKE      (negative)
 *  2003 = LAUGH        (positive)
 *  2004 = EMPHASIZE    (positive)
 *  2005 = QUESTION     (neutral)
 *  2006 = CUSTOM_EMOJI (positive — Apple Sticker / custom-emoji tapback,
 *                       added in macOS 14+; the comment block at
 *                       imessage.c:1015-1018 OMITS this — that comment
 *                       is stale, but the actual switch at line 1830
 *                       handles 2006 and the positive-set filter at
 *                       line 1890 includes it).
 *
 * Apple uses 2000-2006 for "add" reactions and 3000-3006 for "remove"
 * reactions (offset +1000). Our normalizer reports add codes only;
 * the caller sets is_removal based on whether the row came from a 3xxx
 * code. */
hu_error_t hu_reaction_normalize_imessage(int32_t associated_message_type,
                                          hu_reaction_kind_t *out_kind,
                                          hu_reaction_polarity_t *out_polarity);

/* Polarity of a custom-emoji tapback from its glyph: known negative ->
 * NEGATIVE, known positive -> POSITIVE, unknown / absent -> NEUTRAL. */
hu_reaction_polarity_t hu_reaction_emoji_polarity(const char *emoji);

hu_error_t hu_reaction_normalize_slack(const char *reactji_name, hu_reaction_kind_t *out_kind,
                                       hu_reaction_polarity_t *out_polarity);

#ifdef __cplusplus
}
#endif
#endif
