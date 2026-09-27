/* Native Messages voice delivery — pure policy (spec 2026-09-26 W3).
 * No Apple or chat.db dependencies; see include/human/channels/imessage_voice_record.h. */
#include "human/channels/imessage_voice_record.h"

#include <string.h>
#include <strings.h>

hu_voice_delivery_mode_t hu_voice_delivery_mode_parse(const char *s) {
    if (s && strcmp(s, "messages") == 0)
        return HU_VOICE_DELIVERY_MESSAGES;
    if (s && strcmp(s, "shadow") == 0)
        return HU_VOICE_DELIVERY_SHADOW;
    return HU_VOICE_DELIVERY_ATTACHMENT;
}

static bool has_audio_ext(const char *p) {
    static const char *const exts[] = {".caf", ".m4a", ".mp3", ".wav"};
    size_t n = strlen(p);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t e = strlen(exts[i]);
        if (n > e && strcasecmp(p + n - e, exts[i]) == 0)
            return true;
    }
    return false;
}

bool hu_voice_record_is_memo_send(size_t message_len, const char *const *media,
                                  size_t media_count) {
    return message_len == 0 && media && media_count == 1 && media[0] && media[0][0] == '/' &&
           has_audio_ext(media[0]);
}

hu_voice_record_block_t hu_voice_record_preflight(const hu_voice_record_facts_t *f) {
    if (!f || !f->ax_trusted)
        return HU_VREC_NO_AX;
    if (!f->messages_running)
        return HU_VREC_NO_MESSAGES;
    if (!f->blackhole_present)
        return HU_VREC_NO_BLACKHOLE;
    if (!f->real_mic_configured || !f->real_mic_present)
        return HU_VREC_NO_REAL_MIC;
    if (f->real_mic_busy)
        return HU_VREC_MIC_BUSY;
    if (f->user_idle_sec < f->min_idle_sec)
        return HU_VREC_USER_ACTIVE;
    return HU_VREC_OK;
}

const char *hu_voice_record_block_name(hu_voice_record_block_t b) {
    switch (b) {
    case HU_VREC_OK:
        return "ok";
    case HU_VREC_NO_AX:
        return "no_ax";
    case HU_VREC_NO_MESSAGES:
        return "no_messages";
    case HU_VREC_NO_BLACKHOLE:
        return "no_blackhole";
    case HU_VREC_NO_REAL_MIC:
        return "no_real_mic";
    case HU_VREC_MIC_BUSY:
        return "mic_busy";
    case HU_VREC_USER_ACTIVE:
        return "user_active";
    }
    return "unknown";
}

void hu_voice_record_timing(uint32_t seed, hu_voice_record_timing_t *out) {
    if (!out)
        return;
    uint32_t x = seed ? seed : 0x9e3779b9u; /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out->lead_in_ms = 350u + x % 351u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out->tail_ms = 500u + x % 401u;
}

hu_voice_record_route_t hu_voice_record_route(hu_voice_delivery_mode_t mode, size_t message_len,
                                              const char *const *media, size_t media_count) {
    if (mode == HU_VOICE_DELIVERY_ATTACHMENT ||
        !hu_voice_record_is_memo_send(message_len, media, media_count))
        return HU_VREC_ROUTE_ATTACHMENT;
    return mode == HU_VOICE_DELIVERY_SHADOW ? HU_VREC_ROUTE_SHADOW : HU_VREC_ROUTE_RECORD;
}
