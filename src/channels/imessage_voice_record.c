/* Native Messages voice delivery — pure policy (spec 2026-09-26 W3).
 * No Apple or chat.db dependencies; see include/human/channels/imessage_voice_record.h. */
#include "human/channels/imessage_voice_record.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

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

static bool input_is(const hu_voice_record_port_t *p, const char *want) {
    char cur[128] = {0};
    return p->get_input(p->ctx, cur, sizeof(cur)) == HU_OK && strcmp(cur, want) == 0;
}

hu_error_t hu_voice_record_send(const hu_voice_record_port_t *p,
                                const hu_voice_record_request_t *req,
                                hu_voice_record_result_t *out) {
    if (!p || !req || !out || !req->audio_path || !req->handle || !req->real_mic)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->prior_max_rowid = -1;

    hu_voice_record_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    if (p->gather_facts(p->ctx, req->real_mic, &facts) != HU_OK)
        facts.ax_trusted = false;
    facts.min_idle_sec = req->min_idle_sec;
    out->stage = HU_VREC_STAGE_PREFLIGHT;
    out->block = hu_voice_record_preflight(&facts);
    if (out->block != HU_VREC_OK)
        return HU_ERR_NOT_SUPPORTED;

    hu_voice_record_timing_t tm;
    hu_voice_record_timing(req->seed, &tm);
    hu_error_t rc = HU_ERR_IO;
    bool sent = false;

    if (p->remember_ui(p->ctx) != HU_OK)
        goto restore;
    out->prior_max_rowid = p->max_rowid(p->ctx);
    if (p->playback_prepare(p->ctx, req->audio_path) != HU_OK)
        goto restore;
    out->stage = HU_VREC_STAGE_INPUT;
    if (p->set_input(p->ctx, HU_VREC_BLACKHOLE_NAME) != HU_OK ||
        !input_is(p, HU_VREC_BLACKHOLE_NAME))
        goto restore;
    out->stage = HU_VREC_STAGE_OPEN;
    if (p->open_chat(p->ctx, req->handle, req->handle_len) != HU_OK ||
        !p->wait_label(p->ctx, HU_VREC_LABEL_RECORD, 3000))
        goto restore;
    if (p->press(p->ctx, HU_VREC_LABEL_RECORD) != HU_OK)
        goto restore;
    out->stage = HU_VREC_STAGE_RECORD;
    if (!p->wait_label(p->ctx, HU_VREC_LABEL_STOP, 3000))
        goto restore;
    p->sleep_ms(p->ctx, tm.lead_in_ms);
    out->stage = HU_VREC_STAGE_PLAY;
    if (p->playback_run(p->ctx) != HU_OK)
        goto restore;
    p->sleep_ms(p->ctx, tm.tail_ms);
    out->stage = HU_VREC_STAGE_STOP;
    if (p->press(p->ctx, HU_VREC_LABEL_STOP) != HU_OK)
        goto restore;
    /* Messages is still finalizing the recording right after Stop, and a Send
     * pressed then is silently ignored (live test 2026-09-27). */
    p->sleep_ms(p->ctx, HU_VREC_SEND_SETTLE_MS);
    if (!p->wait_label(p->ctx, HU_VREC_LABEL_SEND, 3000))
        goto restore;
    /* A press is not a send: the compose bar returning to "Record audio" is the
     * UI's proof the memo left. One retry, then chat.db decides — pressing
     * Cancel and falling back only when the memo is genuinely not there. */
    for (int attempt = 0; attempt < 2 && !sent; attempt++) {
        if (attempt > 0)
            p->sleep_ms(p->ctx, HU_VREC_SEND_SETTLE_MS);
        if (p->press(p->ctx, HU_VREC_LABEL_SEND) != HU_OK)
            break;
        sent = p->wait_label(p->ctx, HU_VREC_LABEL_RECORD, 3000);
    }
    if (!sent)
        sent = p->audio_row_after(p->ctx, req->handle, req->handle_len, out->prior_max_rowid, 3000);
    if (!sent)
        goto restore;
    out->stage = HU_VREC_STAGE_SENT;
    rc = HU_OK;

restore:
    /* Always: never leave a half-made recording, the mic on BlackHole, or the
     * user's screen on Messages. The real mic is restored by name — not "the
     * previous input" — and read back. */
    if (!sent && out->stage >= HU_VREC_STAGE_RECORD)
        (void)p->press(p->ctx, HU_VREC_LABEL_CANCEL);
    p->playback_dispose(p->ctx);
    (void)p->set_input(p->ctx, req->real_mic);
    out->restored = input_is(p, req->real_mic);
    p->restore_ui(p->ctx);
    if (sent)
        out->verified =
            p->audio_row_after(p->ctx, req->handle, req->handle_len, out->prior_max_rowid, 10000);
    return rc;
}

void hu_voice_record_request_from_env(const char *handle, size_t handle_len, const char *audio_path,
                                      uint32_t seed, hu_voice_record_request_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->handle = handle;
    out->handle_len = handle_len;
    out->audio_path = audio_path;
    const char *mic = getenv("HU_VOICE_REAL_INPUT");
    out->real_mic = mic ? mic : "";
    const char *idle = getenv("HU_VOICE_MIN_IDLE_SEC");
    out->min_idle_sec = (idle && idle[0]) ? atof(idle) : 20.0;
    out->seed = seed;
}

hu_error_t hu_voice_record_send_from_env(const char *handle, size_t handle_len,
                                         const char *audio_path, hu_voice_record_result_t *out) {
    hu_voice_record_request_t req;
    hu_voice_record_request_from_env(handle, handle_len, audio_path,
                                     (uint32_t)time(NULL) ^ (uint32_t)getpid(), &req);
    return hu_voice_record_send(hu_voice_record_macos_port(), &req, out);
}
