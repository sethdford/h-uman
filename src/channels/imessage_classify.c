/*
 * imessage_classify.c — pure lookup / classification helpers for iMessage.
 *
 * This file is the first carve-out from the 4400-LOC imessage.c. The goal is
 * not to be ambitious — these functions are stateless string-mapping helpers
 * with zero platform-specific dependencies. Moving them out demonstrates the
 * mechanics of the larger refactor (new .c file, CMakeLists wiring, header
 * already public) without touching any behavior. ASan / suite identical.
 *
 * What lives here:
 *   - Reaction-type → iMessage tapback name lookup.
 *   - chat.db expressive_send_style_id → friendly effect name.
 *   - chat.db balloon_bundle_id → "[Sticker]" / "[Memoji]" / "[iMessage App]".
 *   - Placeholder-message text detection ([Photo] / [Video] / [Voice Message]).
 *   - Bounded copy utility for fixed-size string fields.
 *
 * What stays in imessage.c (for now):
 *   - `imessage_sanitize_output` — depends on hu_conversation_strip_ai_phrases.
 *   - `imessage_reaction_to_ax_action_prefix` — tightly coupled to the AX
 *     tapback path; will move with that module when imessage_react.c lands.
 *
 * Future carve-outs (see docs/plans/2026-05-12-imessage-shape-refactor.md):
 *   - imessage_watch.c       — imsg subprocess lifecycle (~140 LOC)
 *   - imessage_typing.c      — AX typing + simulated typing (~200 LOC)
 *   - imessage_react.c       — tapback 3-tier fallback (~300 LOC)
 *   - imessage_attachment.c  — GIF/image/media + JSON-safe URL parse (~400 LOC)
 *   - imessage_poll.c        — chat.db poll + schema cache + parsers (~500 LOC)
 *   - imessage_send.c        — text/media send dual-path (imsg / AppleScript)
 */

#include "human/channels/imessage.h"
#include <string.h>

const char *hu_imessage_reaction_to_tapback_name(hu_reaction_type_t reaction) {
    switch (reaction) {
    case HU_REACTION_HEART:
        return "love";
    case HU_REACTION_THUMBS_UP:
        return "like";
    case HU_REACTION_THUMBS_DOWN:
        return "dislike";
    case HU_REACTION_HAHA:
        return "laugh";
    case HU_REACTION_EMPHASIS:
        return "emphasize";
    case HU_REACTION_QUESTION:
        return "question";
    case HU_REACTION_CUSTOM_EMOJI:
        return "emoji";
    default:
        return NULL;
    }
}

const char *hu_imessage_effect_name(const char *style_id) {
    if (!style_id || !style_id[0])
        return NULL;
    if (strstr(style_id, "impact"))
        return "Slam";
    if (strstr(style_id, "loud"))
        return "Loud";
    if (strstr(style_id, "gentle"))
        return "Gentle";
    if (strstr(style_id, "invisibleink"))
        return "Invisible Ink";
    if (strstr(style_id, "CKConfettiEffect"))
        return "Confetti";
    if (strstr(style_id, "CKEchoEffect"))
        return "Echo";
    if (strstr(style_id, "CKFireworksEffect"))
        return "Fireworks";
    if (strstr(style_id, "CKHappyBirthdayEffect"))
        return "Happy Birthday";
    if (strstr(style_id, "CKHeartEffect"))
        return "Heart";
    if (strstr(style_id, "CKLasersEffect"))
        return "Lasers";
    if (strstr(style_id, "CKShootingStarEffect"))
        return "Shooting Star";
    if (strstr(style_id, "CKSparklesEffect"))
        return "Sparkles";
    if (strstr(style_id, "CKSpotlightEffect"))
        return "Spotlight";
    return NULL;
}

const char *hu_imessage_balloon_label(const char *balloon_id) {
    if (!balloon_id || !balloon_id[0])
        return NULL;
    /* Animoji/Memoji checked first: their bundle IDs often contain "Stickers" */
    if (strstr(balloon_id, "Animoji") || strstr(balloon_id, "animoji") ||
        strstr(balloon_id, "Memoji") || strstr(balloon_id, "memoji"))
        return "[Memoji]";
    if (strstr(balloon_id, "Sticker") || strstr(balloon_id, "sticker"))
        return "[Sticker]";
    return "[iMessage App]";
}

bool hu_imessage_text_is_placeholder(const char *text) {
    if (!text || text[0] == '\0')
        return true;
    return strcmp(text, "[Photo]") == 0 || strcmp(text, "[Video]") == 0 ||
           strcmp(text, "[Voice Message]") == 0;
}

size_t hu_imessage_copy_bounded(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    if (!dst || dst_cap == 0)
        return 0;
    if (!src || src_len == 0) {
        dst[0] = '\0';
        return 0;
    }
    size_t cplen = src_len >= dst_cap ? dst_cap - 1 : src_len;
    memcpy(dst, src, cplen);
    dst[cplen] = '\0';
    return cplen;
}

/* AX trust check stub for non-Apple / test builds. The real implementation
 * lives in imessage_ax.c (Apple-only); on every other platform AX permission
 * is meaningless — return false unconditionally so doctor surfaces the
 * "platform doesn't have AX" case rather than crashing on a missing symbol. */
#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__)
bool hu_imessage_ax_is_trusted(void) {
    return false;
}
#endif
