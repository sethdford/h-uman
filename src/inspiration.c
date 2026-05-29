#include "human/inspiration.h"
#include "human/core/string.h"
#include "human/tools/validation.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

hu_inspiration_medium_t hu_inspiration_pick_medium(const char *incoming, size_t incoming_len,
                                                   bool youtube_available) {
    if (!incoming || incoming_len == 0)
        return HU_INSPIRATION_MUSIC;

    static const char *tiktok_cues[] = {"tiktok", "fyp", "for you", "trend", "trending", "reel"};
    for (size_t i = 0; i < sizeof(tiktok_cues) / sizeof(tiktok_cues[0]); i++)
        if (hu_str_contains_word_ci_n(incoming, incoming_len, tiktok_cues[i]))
            return HU_INSPIRATION_TIKTOK;

    static const char *yt_cues[] = {"video",    "watch",   "clip", "youtube",
                                    "tutorial", "trailer", "funny"};
    for (size_t i = 0; i < sizeof(yt_cues) / sizeof(yt_cues[0]); i++)
        if (hu_str_contains_word_ci_n(incoming, incoming_len, yt_cues[i]))
            return youtube_available ? HU_INSPIRATION_YOUTUBE : HU_INSPIRATION_MUSIC;

    return HU_INSPIRATION_MUSIC;
}

const char *hu_inspiration_system_prompt(hu_inspiration_medium_t medium) {
    switch (medium) {
    case HU_INSPIRATION_YOUTUBE:
        return "Suggest ONE YouTube search that fits the conversation. Return exactly:\n"
               "SEARCH QUERY | your brief casual message\n"
               "The message is a natural text in the user's own voice - not a recommendation. "
               "Under 80 chars. No quotes, no URLs.";
    case HU_INSPIRATION_TIKTOK:
        return "Suggest ONE TikTok hashtag keyword that fits the conversation. Return exactly:\n"
               "HASHTAG KEYWORD | your brief casual message\n"
               "Keyword is 1-2 words, no '#'. Message is a natural text in the user's own "
               "voice. Under 80 chars. No quotes, no URLs.";
    case HU_INSPIRATION_MUSIC:
    default:
        return "Suggest ONE song that fits the conversation mood. Return exactly:\n"
               "ARTIST - TITLE | your brief casual message\n"
               "The message is a natural text in the user's own voice - not a recommendation. "
               "Under 80 chars. No quotes, no URLs.";
    }
}

size_t hu_inspiration_build_voice_hint(const char *formality, const char *traits_csv, char *out,
                                       size_t cap) {
    if (!out || cap < 16)
        return 0;
    bool has_f = formality && *formality;
    bool has_t = traits_csv && *traits_csv;
    if (!has_f && !has_t) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, cap, "Write the message in this voice: %s%s%s.", has_f ? formality : "",
                     (has_f && has_t) ? ", " : "", has_t ? traits_csv : "");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t hu_tiktok_tag_url(const char *keyword, size_t keyword_len, char *out, size_t cap) {
    if (!keyword || keyword_len == 0 || !out || cap < 32)
        return 0;
    size_t i = 0;
    while (i < keyword_len && (keyword[i] == '#' || keyword[i] == ' '))
        i++;

    char enc[128];
    size_t e = 0;
    for (; i < keyword_len && e + 4 < sizeof(enc); i++) {
        unsigned char c = (unsigned char)keyword[i];
        if (isalnum(c)) {
            enc[e++] = (char)tolower(c);
        } else if (c == ' ' || c == '-' || c == '_') {
            continue;
        } else {
            int m = snprintf(enc + e, sizeof(enc) - e, "%%%02X", c);
            if (m > 0)
                e += (size_t)m;
        }
    }
    enc[e] = '\0';
    if (e == 0)
        return 0;
    int n = snprintf(out, cap, "https://www.tiktok.com/tag/%s", enc);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

bool hu_inspiration_send_two_bubble(hu_channel_t *channel, const char *target, size_t target_len,
                                    const char *casual_msg, const char *url, unsigned gap_us) {
    if (!channel || !channel->vtable || !channel->vtable->send || !url || !*url)
        return false;
    if (hu_tool_validate_url(url) != HU_OK)
        return false;

    if (casual_msg && *casual_msg) {
        channel->vtable->send(channel->ctx, target, target_len, casual_msg, strlen(casual_msg),
                              NULL, 0);
        if (gap_us > 0)
            usleep(gap_us);
    }
    channel->vtable->send(channel->ctx, target, target_len, url, strlen(url), NULL, 0);
    return true;
}
