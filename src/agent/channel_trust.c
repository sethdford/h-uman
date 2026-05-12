/*
 * SOTA-2026 init-09 §2.10: channel → trust-tier classifier.
 *
 * The classifier is the single source of truth for converting an
 * `agent->active_channel` string into an `hu_trust_tier_t`. Channel
 * handlers MUST emit *qualified* strings ("telegram_dm" vs "telegram_group")
 * — see security review 09-M3 in adversarial-review-security.md. An
 * unqualified or unknown channel falls through to THIRD_PARTY, the safe
 * default.
 */

#include "human/agent/channel_trust.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static bool starts_with_ci(const char *s, size_t s_len, const char *p) {
    size_t pl = strlen(p);
    if (s_len < pl)
        return false;
    for (size_t i = 0; i < pl; i++) {
        char a = (char)tolower((unsigned char)s[i]);
        char b = (char)tolower((unsigned char)p[i]);
        if (a != b)
            return false;
    }
    return true;
}

static bool equals_ci(const char *s, size_t s_len, const char *p) {
    size_t pl = strlen(p);
    if (s_len != pl)
        return false;
    return starts_with_ci(s, s_len, p);
}

hu_trust_tier_t hu_channel_trust(const char *channel, size_t channel_len) {
    if (!channel || channel_len == 0)
        return HU_TRUST_THIRD_PARTY;

    /* USER_DIRECT — verified 1:1 sessions. */
    if (equals_ci(channel, channel_len, "cli") ||
        equals_ci(channel, channel_len, "stdin"))
        return HU_TRUST_USER_DIRECT;
    if (equals_ci(channel, channel_len, "telegram_dm") ||
        equals_ci(channel, channel_len, "discord_dm") ||
        equals_ci(channel, channel_len, "slack_dm") ||
        equals_ci(channel, channel_len, "imessage_dm") ||
        equals_ci(channel, channel_len, "imessage"))
        return HU_TRUST_USER_DIRECT;

    /* PERSONA_DERIVED — computed from the user's own outputs. */
    if (starts_with_ci(channel, channel_len, "persona_derived") ||
        starts_with_ci(channel, channel_len, "rem-synthesis"))
        return HU_TRUST_PERSONA_DERIVED;

    /* FIRST_PARTY — user-installed tools and self-origin feeds. */
    if (equals_ci(channel, channel_len, "human") ||
        equals_ci(channel, channel_len, "tool:human") ||
        equals_ci(channel, channel_len, "self_email") ||
        equals_ci(channel, channel_len, "calendar_self") ||
        starts_with_ci(channel, channel_len, "self:") ||
        starts_with_ci(channel, channel_len, "tool:"))
        return HU_TRUST_FIRST_PARTY;

    /* THIRD_PARTY — group chats, external feeds, social, news, others' email. */
    if (starts_with_ci(channel, channel_len, "feed:") ||
        starts_with_ci(channel, channel_len, "rss") ||
        starts_with_ci(channel, channel_len, "news") ||
        starts_with_ci(channel, channel_len, "twitter") ||
        starts_with_ci(channel, channel_len, "facebook") ||
        starts_with_ci(channel, channel_len, "instagram") ||
        starts_with_ci(channel, channel_len, "social"))
        return HU_TRUST_THIRD_PARTY;
    if (starts_with_ci(channel, channel_len, "telegram_group") ||
        starts_with_ci(channel, channel_len, "discord_channel") ||
        starts_with_ci(channel, channel_len, "discord_server") ||
        starts_with_ci(channel, channel_len, "slack_channel") ||
        starts_with_ci(channel, channel_len, "imessage_group"))
        return HU_TRUST_THIRD_PARTY;

    /* Unqualified strings ("telegram", "discord", "slack") are dangerous
     * — they may be 1:1 OR group. Treat as THIRD_PARTY (safe default) and
     * surface via the recall verifier's "[Unverified hints]" path. The
     * channel handler should be patched to emit a qualified string. */
    return HU_TRUST_THIRD_PARTY;
}

bool hu_channel_is_one_to_one(const char *channel, size_t channel_len) {
    return hu_channel_trust(channel, channel_len) >= HU_TRUST_USER_DIRECT;
}

hu_provenance_t hu_channel_trust_stamp(const char *channel, size_t channel_len,
                                       const char *handle, size_t handle_len,
                                       int64_t now_ts) {
    hu_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.tier = hu_channel_trust(channel, channel_len);
    p.source_ts = now_ts;
    if (channel && channel_len > 0) {
        size_t n = channel_len < sizeof(p.channel) - 1 ? channel_len
                                                        : sizeof(p.channel) - 1;
        memcpy(p.channel, channel, n);
        p.channel[n] = '\0';
    } else {
        const char fallback[] = "unknown";
        memcpy(p.channel, fallback, sizeof(fallback));
    }
    if (handle && handle_len > 0) {
        size_t n = handle_len < sizeof(p.contact_handle) - 1
                       ? handle_len
                       : sizeof(p.contact_handle) - 1;
        memcpy(p.contact_handle, handle, n);
        p.contact_handle[n] = '\0';
    }
    return p;
}
