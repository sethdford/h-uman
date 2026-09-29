/* Native Messages voice delivery — pure policy (spec 2026-09-26 W3).
 * No Apple or chat.db dependencies; see include/human/channels/imessage_voice_record.h. */
#include "human/channels/imessage_voice_record.h"

#include <ctype.h>
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
    /* A call on AirPods leaves the configured mic idle; switching the default
     * input would hand that call the clip. Only run when the default input is
     * the real mic, so the restore puts back exactly what was there. */
    if (!f->default_input_is_real_mic)
        return HU_VREC_INPUT_NOT_REAL_MIC;
    /* Something else already plays into BlackHole: it would be recorded too. */
    if (f->blackhole_busy)
        return HU_VREC_BLACKHOLE_BUSY;
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
    case HU_VREC_INPUT_NOT_REAL_MIC:
        return "input_not_real_mic";
    case HU_VREC_BLACKHOLE_BUSY:
        return "blackhole_busy";
    case HU_VREC_BAD_HANDLE:
        return "bad_handle";
    case HU_VREC_LOCKED:
        return "locked";
    case HU_VREC_UNKNOWN_CHAT:
        return "unknown_chat";
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

bool hu_voice_record_may_attach(size_t message_len, const char *const *media, size_t media_count,
                                const char *no_attachment_env) {
    bool refuse = no_attachment_env && strcmp(no_attachment_env, "1") == 0;
    return !(refuse && hu_voice_record_is_memo_send(message_len, media, media_count));
}

hu_voice_record_route_t hu_voice_record_route(hu_voice_delivery_mode_t mode, size_t message_len,
                                              const char *const *media, size_t media_count) {
    if (mode == HU_VOICE_DELIVERY_ATTACHMENT ||
        !hu_voice_record_is_memo_send(message_len, media, media_count))
        return HU_VREC_ROUTE_ATTACHMENT;
    return mode == HU_VOICE_DELIVERY_SHADOW ? HU_VREC_ROUTE_SHADOW : HU_VREC_ROUTE_RECORD;
}

bool hu_voice_record_handle_ok(const char *h, size_t n) {
    if (!h || n == 0 || n > 254)
        return false;
    size_t at = 0, digits = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if (c == '@')
            at++;
        else if (c >= '0' && c <= '9')
            digits++;
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                   c == '-' || c == '+' || c == '%'))
            return false; /* no ';', '?', '#', '/', ':', spaces, ... */
    }
    if (at == 1)
        return h[0] != '@' && h[n - 1] != '@';
    if (at > 1)
        return false;
    /* phone: an optional leading '+', then digits only */
    for (size_t i = (h[0] == '+') ? 1 : 0; i < n; i++)
        if (h[i] < '0' || h[i] > '9')
            return false;
    return digits >= 7;
}

static void trim_span(const char *s, const char **b, const char **e) {
    *b = s;
    *e = s + strlen(s);
    while (*b < *e && isspace((unsigned char)**b))
        (*b)++;
    while (*e > *b && isspace((unsigned char)(*e)[-1]))
        (*e)--;
}

bool hu_voice_record_title_matches(const char *window_title, const char *expected_title) {
    if (!window_title || !expected_title)
        return false;
    const char *wb, *we, *xb, *xe;
    trim_span(window_title, &wb, &we);
    trim_span(expected_title, &xb, &xe);
    size_t wn = (size_t)(we - wb), xn = (size_t)(xe - xb);
    return wn > 0 && wn == xn && strncasecmp(wb, xb, wn) == 0;
}

static bool in_target_chat(const hu_voice_record_port_t *p, const char *expected) {
    char title[256] = {0};
    return p->chat_title(p->ctx, title, sizeof(title)) == HU_OK &&
           hu_voice_record_title_matches(title, expected);
}

/* Any keyboard/mouse input since the run began means the user is back: text
 * they type would land in the target's compose field. HU_VOICE_MIN_IDLE_SEC=0
 * is the operator's "I'm testing at the Mac" override. */
static bool user_returned(const hu_voice_record_port_t *p, const hu_voice_record_request_t *req,
                          uint64_t t0_ms) {
    if (req->min_idle_sec <= 0.0)
        return false;
    uint64_t now = p->now_ms(p->ctx);
    double elapsed = now > t0_ms ? (double)(now - t0_ms) / 1000.0 : 0.0;
    return p->idle_sec(p->ctx) < elapsed;
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

    /* Blocks below touch nothing: no input switch, no UI, no lock left held. */
    if (!hu_voice_record_handle_ok(req->handle, req->handle_len)) {
        out->block = HU_VREC_BAD_HANDLE;
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!p->try_lock(p->ctx)) {
        out->block = HU_VREC_LOCKED;
        return HU_ERR_NOT_SUPPORTED;
    }
    hu_voice_record_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    if (p->gather_facts(p->ctx, req->real_mic, &facts) != HU_OK)
        facts.ax_trusted = false;
    facts.min_idle_sec = req->min_idle_sec;
    out->stage = HU_VREC_STAGE_PREFLIGHT;
    out->block = hu_voice_record_preflight(&facts);
    /* A patient request (the owner's self-test) waits for the user to step
     * away rather than falling back at once; nothing is touched meanwhile. */
    while (out->block == HU_VREC_USER_ACTIVE && out->idle_waited_ms < req->idle_wait_ms) {
        p->sleep_ms(p->ctx, HU_VREC_IDLE_POLL_MS);
        out->idle_waited_ms += HU_VREC_IDLE_POLL_MS;
        facts.user_idle_sec = p->idle_sec(p->ctx);
        out->block = hu_voice_record_preflight(&facts);
    }
    /* The wrong-recipient guard's reference: the name Messages itself shows for
     * the target. Without it the open chat cannot be confirmed — never record. */
    char expected[256] = {0};
    if (out->block == HU_VREC_OK && (p->expected_title(p->ctx, req->handle, req->handle_len,
                                                       expected, sizeof(expected)) != HU_OK ||
                                     !expected[0]))
        out->block = HU_VREC_UNKNOWN_CHAT;
    if (out->block != HU_VREC_OK) {
        p->unlock(p->ctx);
        return HU_ERR_NOT_SUPPORTED;
    }

    hu_voice_record_timing_t tm;
    hu_voice_record_timing(req->seed, &tm);
    uint64_t t0 = p->now_ms(p->ctx);
    hu_error_t rc = HU_ERR_IO;
    bool sent = false;
    bool cancelled = false;

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
    /* "Record audio" exists in every conversation: confirm it is the target's. */
    if (!in_target_chat(p, expected)) {
        out->abort_reason = "wrong_chat";
        goto restore;
    }
    /* Staged before the press: AX can report an error after the recording has
     * started, and the restore path must still cancel it. */
    out->stage = HU_VREC_STAGE_RECORD;
    if (p->press(p->ctx, HU_VREC_LABEL_RECORD) != HU_OK ||
        !p->wait_label(p->ctx, HU_VREC_LABEL_STOP, 3000))
        goto restore;
    p->sleep_ms(p->ctx, tm.lead_in_ms);
    out->stage = HU_VREC_STAGE_PLAY;
    if (p->playback_run(p->ctx) != HU_OK)
        goto restore;
    p->sleep_ms(p->ctx, tm.tail_ms);
    if (user_returned(p, req, t0)) {
        out->abort_reason = "user_returned";
        goto restore;
    }
    out->stage = HU_VREC_STAGE_STOP;
    if (p->press(p->ctx, HU_VREC_LABEL_STOP) != HU_OK)
        goto restore;
    /* Messages is still finalizing the recording right after Stop, and a Send
     * pressed then is silently ignored (live test 2026-09-27). */
    p->sleep_ms(p->ctx, HU_VREC_SEND_SETTLE_MS);
    if (!p->wait_label(p->ctx, HU_VREC_LABEL_SEND, 3000))
        goto restore;
    if (!in_target_chat(p, expected)) {
        out->abort_reason = "wrong_chat";
        goto restore;
    }
    if (user_returned(p, req, t0)) {
        out->abort_reason = "user_returned";
        goto restore;
    }
    /* A press is not a send: the compose bar returning to "Record audio" is the
     * UI's proof the memo left. One retry, then chat.db decides. */
    for (int attempt = 0; attempt < 2 && !sent; attempt++) {
        if (attempt > 0)
            p->sleep_ms(p->ctx, HU_VREC_SEND_SETTLE_MS);
        if (p->press(p->ctx, HU_VREC_LABEL_SEND) != HU_OK)
            break;
        sent = p->wait_label(p->ctx, HU_VREC_LABEL_RECORD, 3000);
    }
    if (!sent)
        sent = p->audio_row_after(p->ctx, req->handle, req->handle_len, out->prior_max_rowid, 3000);
    if (!sent) {
        /* Fall back to the attachment only when the memo is provably still in
         * the compose bar (Cancel worked). If there is nothing to cancel it
         * left, unconfirmed — sending the file too would duplicate it. */
        if (p->press(p->ctx, HU_VREC_LABEL_CANCEL) == HU_OK) {
            cancelled = true;
            out->abort_reason = "send_unconfirmed";
            goto restore;
        }
        sent = true;
    }
    out->stage = HU_VREC_STAGE_SENT;
    rc = HU_OK;

restore:
    /* Always: never leave a half-made recording, the mic on BlackHole, or the
     * user's screen on Messages. The real mic is restored by name — not "the
     * previous input" — and read back. */
    if (!sent && !cancelled && out->stage >= HU_VREC_STAGE_RECORD &&
        p->press(p->ctx, HU_VREC_LABEL_CANCEL) != HU_OK)
        out->cancel_failed = true;
    p->playback_dispose(p->ctx);
    (void)p->set_input(p->ctx, req->real_mic);
    out->restored = input_is(p, req->real_mic);
    p->restore_ui(p->ctx);
    if (sent)
        out->verified =
            p->audio_row_after(p->ctx, req->handle, req->handle_len, out->prior_max_rowid, 10000);
    p->unlock(p->ctx);
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
    /* handle_allowed treats an empty list as "everyone"; here it means no one. */
    const char *patient = getenv("HU_VOICE_IDLE_WAIT_HANDLES");
    if (patient && patient[0] && hu_voice_record_handle_allowed(patient, handle, handle_len)) {
        const char *wait = getenv("HU_VOICE_IDLE_WAIT_SEC");
        double sec = (wait && wait[0]) ? atof(wait) : 90.0;
        out->idle_wait_ms = sec > 0.0 && sec < 600.0 ? (uint32_t)(sec * 1000.0) : 0;
    }
}

hu_error_t hu_voice_record_send_from_env(const char *handle, size_t handle_len,
                                         const char *audio_path, hu_voice_record_result_t *out) {
    hu_voice_record_request_t req;
    hu_voice_record_request_from_env(handle, handle_len, audio_path,
                                     (uint32_t)time(NULL) ^ (uint32_t)getpid(), &req);
    return hu_voice_record_send(hu_voice_record_macos_port(), &req, out);
}

bool hu_voice_record_handle_allowed(const char *allow, const char *handle, size_t handle_len) {
    if (!allow || !allow[0])
        return true;
    if (!handle || handle_len == 0)
        return false;
    for (const char *p = allow; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        const char *e = p;
        while (*e && *e != ',')
            e++;
        size_t n = (size_t)(e - p);
        while (n > 0 && p[n - 1] == ' ')
            n--;
        if (n == handle_len && strncasecmp(p, handle, n) == 0)
            return true;
        p = e;
    }
    return false;
}
