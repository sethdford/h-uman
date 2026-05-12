/* src/channels/reaction_event.c */
#include "human/channels/reaction_event.h"
#include <string.h>

hu_error_t hu_reaction_normalize_imessage(int32_t code,
                                          hu_reaction_kind_t *kind,
                                          hu_reaction_polarity_t *polarity) {
    if (!kind || !polarity) return HU_ERR_INVALID_ARGUMENT;
    /* AUTHORITY (full set): src/channels/imessage.c:1812-1832 (switch table)
     * + line 1890 ("positive tapbacks ... 2000-2004,2006") + line 1783
     * (SQL `BETWEEN 2000 AND 2006`). The comment block at imessage.c:1017
     * stops at 2005 — that comment is stale; the live code uses 2006 too.
     *
     * Apple uses 2000-2006 for "add" reactions and 3000-3006 for "remove"
     * reactions (offset +1000). Normalizer reports add-codes only; caller
     * sets is_removal based on whether the row came from a 3xxx code.
     *
     * NOTE: v1 of this plan incorrectly mapped 2003 to DISLIKE and ignored
     * 2006 entirely — those were critical bugs that would have trained
     * negative DPO signals on laughs and silently dropped every custom-emoji
     * tapback. Verified against imessage.c during the spec-verifier gate. */
    int32_t base = code >= 3000 ? code - 1000 : code;
    switch (base) {
        case 2000: *kind = HU_REACTION_LOVE;         *polarity = HU_REACTION_POSITIVE; return HU_OK;
        case 2001: *kind = HU_REACTION_LIKE;         *polarity = HU_REACTION_POSITIVE; return HU_OK;
        case 2002: *kind = HU_REACTION_DISLIKE;      *polarity = HU_REACTION_NEGATIVE; return HU_OK;
        case 2003: *kind = HU_REACTION_LAUGH;        *polarity = HU_REACTION_POSITIVE; return HU_OK;
        case 2004: *kind = HU_REACTION_EMPHASIZE;    *polarity = HU_REACTION_POSITIVE; return HU_OK;
        case 2005: *kind = HU_REACTION_QUESTION;     *polarity = HU_REACTION_NEUTRAL;  return HU_OK;
        case 2006: *kind = HU_REACTION_CUSTOM_EMOJI; *polarity = HU_REACTION_POSITIVE; return HU_OK;
        default:   *kind = HU_REACTION_UNKNOWN;      *polarity = HU_REACTION_NEUTRAL;  return HU_ERR_INVALID_ARGUMENT;
    }
}

hu_error_t hu_reaction_normalize_slack(const char *name,
                                       hu_reaction_kind_t *kind,
                                       hu_reaction_polarity_t *polarity) {
    if (!name || !kind || !polarity) return HU_ERR_INVALID_ARGUMENT;
    if (strcmp(name, "+1") == 0 || strcmp(name, "thumbsup") == 0) {
        *kind = HU_REACTION_LIKE; *polarity = HU_REACTION_POSITIVE; return HU_OK;
    }
    if (strcmp(name, "-1") == 0 || strcmp(name, "thumbsdown") == 0) {
        *kind = HU_REACTION_DISLIKE; *polarity = HU_REACTION_NEGATIVE; return HU_OK;
    }
    if (strcmp(name, "heart") == 0 || strcmp(name, "heart_eyes") == 0) {
        *kind = HU_REACTION_LOVE; *polarity = HU_REACTION_POSITIVE; return HU_OK;
    }
    if (strcmp(name, "joy") == 0 || strcmp(name, "laughing") == 0) {
        *kind = HU_REACTION_LAUGH; *polarity = HU_REACTION_POSITIVE; return HU_OK;
    }
    if (strcmp(name, "thinking_face") == 0 || strcmp(name, "question") == 0) {
        *kind = HU_REACTION_QUESTION; *polarity = HU_REACTION_NEUTRAL; return HU_OK;
    }
    if (strcmp(name, "exclamation") == 0 || strcmp(name, "bangbang") == 0) {
        *kind = HU_REACTION_EMPHASIZE; *polarity = HU_REACTION_POSITIVE; return HU_OK;
    }
    *kind = HU_REACTION_UNKNOWN; *polarity = HU_REACTION_NEUTRAL;
    return HU_ERR_INVALID_ARGUMENT;
}
