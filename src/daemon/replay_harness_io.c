/* replay_harness_io.c — the replay harness's boundary with the outside world:
 * the loopback guard, the null channel (nothing can be sent) and the pinning
 * provider wrapper (fixed model/temperature, request fingerprints).
 * Contract: include/human/daemon/replay_turn.h. */
#include "human/daemon/replay_turn.h"

#include "human/providers/factory.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Loopback guard ──────────────────────────────────────────────────── */

static bool port_ok(const char *p, const char *end) {
    if (p == end)
        return true; /* no port */
    if (*p != ':')
        return false;
    p++;
    size_t digits = 0;
    for (; p < end; p++, digits++) {
        if (!isdigit((unsigned char)*p))
            return false;
    }
    return digits >= 1 && digits <= 5;
}

bool hu_replay_url_is_loopback(const char *url) {
    if (!url)
        return false;
    const char *rest = NULL;
    if (strncasecmp(url, "http://", 7) == 0)
        rest = url + 7;
    else if (strncasecmp(url, "https://", 8) == 0)
        rest = url + 8;
    else
        return false;
    const char *end = rest + strcspn(rest, "/?#");
    if (memchr(rest, '@', (size_t)(end - rest)))
        return false; /* userinfo: what looks like the host may not be */
    if (*rest == '[') {
        const char *close = memchr(rest, ']', (size_t)(end - rest));
        if (!close || (size_t)(close - rest - 1) != 3 || memcmp(rest + 1, "::1", 3) != 0)
            return false;
        return port_ok(close + 1, end);
    }
    const char *colon = memchr(rest, ':', (size_t)(end - rest));
    const char *host_end = colon ? colon : end;
    size_t host_len = (size_t)(host_end - rest);
    bool host_ok = (host_len == 9 && memcmp(rest, "127.0.0.1", 9) == 0) ||
                   (host_len == 9 && strncasecmp(rest, "localhost", 9) == 0);
    return host_ok && port_ok(host_end, end);
}

/* ── Null channel ────────────────────────────────────────────────────── */

static hu_error_t rc_start(void *ctx) {
    (void)ctx;
    return HU_OK;
}
static void rc_stop(void *ctx) {
    (void)ctx;
}
static const char *rc_name(void *ctx) {
    (void)ctx;
    return "imessage";
}
static bool rc_health(void *ctx) {
    (void)ctx;
    return true;
}
static hu_error_t rc_send(void *ctx, const char *target, size_t target_len, const char *message,
                          size_t message_len, const char *const *media, size_t media_count) {
    (void)target, (void)target_len, (void)message, (void)message_len, (void)media,
        (void)media_count;
    ((hu_replay_channel_t *)ctx)->outbound_calls++;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t rc_send_event(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count, hu_outbound_stage_t stage) {
    (void)stage;
    return rc_send(ctx, target, target_len, message, message_len, media, media_count);
}
static hu_error_t rc_typing(void *ctx, const char *recipient, size_t recipient_len) {
    (void)recipient, (void)recipient_len;
    ((hu_replay_channel_t *)ctx)->typing_calls++;
    return HU_OK;
}
static hu_error_t rc_history(void *ctx, hu_allocator_t *alloc, const char *contact_id,
                             size_t contact_id_len, size_t limit, hu_channel_history_entry_t **out,
                             size_t *out_count) {
    (void)contact_id, (void)contact_id_len;
    hu_replay_channel_t *rc = (hu_replay_channel_t *)ctx;
    *out = NULL;
    *out_count = 0;
    size_t n = rc->history_count;
    if (limit > 0 && n > limit)
        n = limit;
    if (!rc->history || n == 0)
        return HU_OK;
    /* The newest `n`, oldest first — what chat.db's loader returns. */
    const hu_channel_history_entry_t *src = rc->history + (rc->history_count - n);
    hu_channel_history_entry_t *copy =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(*copy));
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, src, n * sizeof(*copy));
    *out = copy;
    *out_count = n;
    return HU_OK;
}
static hu_error_t rc_constraints(void *ctx, hu_channel_response_constraints_t *out) {
    out->max_chars = ((hu_replay_channel_t *)ctx)->max_chars;
    return HU_OK;
}
static hu_error_t rc_react(void *ctx, const char *target, size_t target_len, int64_t message_id,
                           hu_reaction_type_t reaction) {
    (void)target, (void)target_len, (void)message_id, (void)reaction;
    ((hu_replay_channel_t *)ctx)->outbound_calls++;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t rc_reply(void *ctx, const char *target, size_t target_len, const char *guid,
                           size_t guid_len, const char *body, size_t body_len) {
    (void)target, (void)target_len, (void)guid, (void)guid_len, (void)body, (void)body_len;
    ((hu_replay_channel_t *)ctx)->outbound_calls++;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t rc_react_emoji(void *ctx, const char *target, size_t target_len,
                                 int64_t message_id, const char *emoji, size_t emoji_len) {
    (void)target, (void)target_len, (void)message_id, (void)emoji, (void)emoji_len;
    ((hu_replay_channel_t *)ctx)->outbound_calls++;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t rc_sticker(void *ctx, const char *target, size_t target_len, const char *path,
                             size_t path_len) {
    (void)target, (void)target_len, (void)path, (void)path_len;
    ((hu_replay_channel_t *)ctx)->outbound_calls++;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t rc_mark_read(void *ctx, const char *contact_id, size_t contact_id_len) {
    (void)contact_id, (void)contact_id_len;
    ((hu_replay_channel_t *)ctx)->mark_read_calls++;
    return HU_OK;
}

static const hu_channel_vtable_t k_replay_channel_vtable = {
    .start = rc_start,
    .stop = rc_stop,
    .send = rc_send,
    .name = rc_name,
    .health_check = rc_health,
    .send_event = rc_send_event,
    .start_typing = rc_typing,
    .stop_typing = rc_typing,
    .load_conversation_history = rc_history,
    .get_response_constraints = rc_constraints,
    .react = rc_react,
    .reply = rc_reply,
    .react_emoji = rc_react_emoji,
    .send_sticker = rc_sticker,
    .mark_read = rc_mark_read,
};

void hu_replay_channel_init(hu_replay_channel_t *rc, const hu_channel_history_entry_t *history,
                            size_t history_count) {
    if (!rc)
        return;
    memset(rc, 0, sizeof(*rc));
    rc->history = history;
    rc->history_count = history ? history_count : 0;
    rc->max_chars = 200; /* imessage_get_response_constraints */
}

hu_channel_t hu_replay_channel_as_channel(hu_replay_channel_t *rc) {
    hu_channel_t ch = {.ctx = rc, .vtable = &k_replay_channel_vtable};
    return ch;
}

/* ── Pinning provider wrapper ────────────────────────────────────────── */

#define RP_FNV_OFFSET  1469598103934665603ULL
#define RP_FNV_PRIME   1099511628211ULL
#define RP_CAPTURE_MAX (512u * 1024u)

static uint64_t rp_fnv(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= RP_FNV_PRIME;
    }
    return h;
}

static void rp_capture_append(hu_replay_provider_t *rp, const char *s, size_t n) {
    if (!rp->capture || !s || n == 0 || rp->captured_len + n + 1 > RP_CAPTURE_MAX)
        return;
    char *grown = (char *)realloc(rp->captured, rp->captured_len + n + 1);
    if (!grown)
        return;
    memcpy(grown + rp->captured_len, s, n);
    rp->captured_len += n;
    grown[rp->captured_len] = '\0';
    rp->captured = grown;
}

static const char *rp_role_name(hu_role_t role) {
    static const char *const k_names[] = {"system", "user", "assistant", "tool"};
    return (unsigned)role < sizeof(k_names) / sizeof(k_names[0]) ? k_names[role] : "unknown";
}

/* Fingerprint (and optionally capture) one reply request: every message's
 * role and content, then max_tokens, stop-sequence and tool counts. */
static void rp_record_reply(hu_replay_provider_t *rp, const hu_chat_request_t *req) {
    rp->reply_calls++;
    uint64_t h = RP_FNV_OFFSET;
    size_t bytes = 0;
    size_t sys_bytes = 0;
    free(rp->captured);
    rp->captured = NULL;
    rp->captured_len = 0;
    for (size_t i = 0; req && req->messages && i < req->messages_count; i++) {
        const hu_chat_message_t *m = &req->messages[i];
        unsigned char role = (unsigned char)m->role;
        h = rp_fnv(h, &role, 1);
        if (m->content && m->content_len > 0)
            h = rp_fnv(h, m->content, m->content_len);
        h = rp_fnv(h, "\0", 1);
        bytes += m->content_len;
        if (m->role == HU_ROLE_SYSTEM)
            sys_bytes += m->content_len;
        const char *rn = rp_role_name(m->role);
        rp_capture_append(rp, "[", 1);
        rp_capture_append(rp, rn, strlen(rn));
        rp_capture_append(rp, "]\n", 2);
        rp_capture_append(rp, m->content, m->content ? m->content_len : 0);
        rp_capture_append(rp, "\n\n", 2);
    }
    if (req) {
        uint64_t tail[3] = {req->max_tokens, req->stop_sequences ? req->stop_sequences_count : 0,
                            req->tools ? req->tools_count : 0};
        h = rp_fnv(h, tail, sizeof(tail));
    }
    rp->reply_fp = h;
    rp->reply_bytes = bytes;
    rp->reply_system_bytes = sys_bytes;
}

static const char *rp_model(hu_replay_provider_t *rp, const char *model, size_t model_len,
                            size_t *out_len) {
    if (rp->model[0]) {
        *out_len = strlen(rp->model);
        return rp->model;
    }
    *out_len = model_len;
    return model;
}

static double rp_temp(const hu_replay_provider_t *rp, double temperature) {
    return rp->force_temperature ? rp->temperature : temperature;
}

/* A copy of the request with the pinned model/temperature. */
static hu_chat_request_t rp_pin_request(hu_replay_provider_t *rp, const hu_chat_request_t *req) {
    hu_chat_request_t pinned = *req;
    if (rp->model[0]) {
        pinned.model = rp->model;
        pinned.model_len = strlen(rp->model);
    }
    pinned.temperature = rp_temp(rp, req->temperature);
    return pinned;
}

static hu_error_t rp_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
                                      size_t system_prompt_len, const char *message,
                                      size_t message_len, const char *model, size_t model_len,
                                      double temperature, char **out, size_t *out_len) {
    hu_replay_provider_t *rp = (hu_replay_provider_t *)ctx;
    rp->calls++;
    size_t ml = 0;
    const char *m = rp_model(rp, model, model_len, &ml);
    return rp->inner.vtable->chat_with_system(rp->inner.ctx, alloc, system_prompt,
                                              system_prompt_len, message, message_len, m, ml,
                                              rp_temp(rp, temperature), out, out_len);
}

/* Every reply call: count it, pin model/temperature on a copy of the request,
 * fingerprint the copy, and resolve the model name to forward. */
static hu_error_t rp_begin_reply(hu_replay_provider_t *rp, const hu_chat_request_t *req,
                                 const char *model, size_t model_len, hu_chat_request_t *pinned,
                                 const char **m, size_t *ml) {
    rp->calls++;
    if (!req)
        return HU_ERR_INVALID_ARGUMENT;
    *pinned = rp_pin_request(rp, req);
    rp_record_reply(rp, pinned);
    *m = rp_model(rp, model, model_len, ml);
    return HU_OK;
}

static hu_error_t rp_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                          const char *model, size_t model_len, double temperature,
                          hu_chat_response_t *out) {
    hu_replay_provider_t *rp = (hu_replay_provider_t *)ctx;
    hu_chat_request_t pinned;
    const char *m = NULL;
    size_t ml = 0;
    hu_error_t err = rp_begin_reply(rp, request, model, model_len, &pinned, &m, &ml);
    return err != HU_OK ? err
                        : rp->inner.vtable->chat(rp->inner.ctx, alloc, &pinned, m, ml,
                                                 rp_temp(rp, temperature), out);
}

static hu_error_t rp_chat_with_tools(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                                     hu_chat_response_t *out) {
    hu_replay_provider_t *rp = (hu_replay_provider_t *)ctx;
    hu_chat_request_t pinned;
    const char *m = NULL;
    size_t ml = 0;
    hu_error_t err = rp_begin_reply(rp, req, NULL, 0, &pinned, &m, &ml);
    (void)m;
    return err != HU_OK ? err
                        : rp->inner.vtable->chat_with_tools(rp->inner.ctx, alloc, &pinned, out);
}

static hu_error_t rp_stream_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                                 const char *model, size_t model_len, double temperature,
                                 hu_stream_callback_t callback, void *callback_ctx,
                                 hu_stream_chat_result_t *out) {
    hu_replay_provider_t *rp = (hu_replay_provider_t *)ctx;
    hu_chat_request_t pinned;
    const char *m = NULL;
    size_t ml = 0;
    hu_error_t err = rp_begin_reply(rp, request, model, model_len, &pinned, &m, &ml);
    if (err != HU_OK)
        return err;
    return rp->inner.vtable->stream_chat(rp->inner.ctx, alloc, &pinned, m, ml,
                                         rp_temp(rp, temperature), callback, callback_ctx, out);
}

static bool rp_supports_native_tools(void *ctx) {
    hu_replay_provider_t *rp = (hu_replay_provider_t *)ctx;
    return rp->inner.vtable->supports_native_tools(rp->inner.ctx);
}
static const char *rp_get_name(void *ctx) {
    hu_replay_provider_t *rp = (hu_replay_provider_t *)ctx;
    return rp->inner.vtable->get_name(rp->inner.ctx);
}
/* The agent's deinit must not free the inner provider: the wrapper owns that
 * decision (hu_replay_provider_deinit). */
static void rp_deinit_noop(void *ctx, hu_allocator_t *alloc) {
    (void)ctx, (void)alloc;
}
static bool rp_supports_streaming(void *ctx) {
    hu_replay_provider_t *rp = (hu_replay_provider_t *)ctx;
    return rp->inner.vtable->supports_streaming(rp->inner.ctx);
}

void hu_replay_provider_init(hu_replay_provider_t *rp, hu_provider_t inner, bool owns_inner,
                             const char *model, bool force_temperature, double temperature) {
    if (!rp)
        return;
    memset(rp, 0, sizeof(*rp));
    rp->inner = inner;
    rp->owns_inner = owns_inner;
    if (model && model[0]) {
        size_t n = strlen(model);
        if (n >= sizeof(rp->model))
            n = sizeof(rp->model) - 1;
        memcpy(rp->model, model, n);
        rp->model[n] = '\0';
    }
    rp->force_temperature = force_temperature;
    rp->temperature = temperature;
    if (!inner.vtable)
        return;
    /* Start from the inner table so optional entries stay present/absent
     * exactly as the real provider has them; wrap the ones that talk to it. */
    rp->vtable = *inner.vtable;
    rp->vtable.chat_with_system = inner.vtable->chat_with_system ? rp_chat_with_system : NULL;
    rp->vtable.chat = inner.vtable->chat ? rp_chat : NULL;
    rp->vtable.chat_with_tools = inner.vtable->chat_with_tools ? rp_chat_with_tools : NULL;
    rp->vtable.stream_chat = inner.vtable->stream_chat ? rp_stream_chat : NULL;
    rp->vtable.supports_native_tools =
        inner.vtable->supports_native_tools ? rp_supports_native_tools : NULL;
    rp->vtable.get_name = inner.vtable->get_name ? rp_get_name : NULL;
    rp->vtable.supports_streaming = inner.vtable->supports_streaming ? rp_supports_streaming : NULL;
    rp->vtable.deinit = rp_deinit_noop;
    /* Entries that take the inner ctx directly would receive the wrapper's;
     * none of them is on the reply path, so drop them rather than forward. */
    rp->vtable.warmup = NULL;
    rp->vtable.supports_vision = NULL;
    rp->vtable.supports_vision_for_model = NULL;
    rp->vtable.load_adapter = NULL;
    rp->vtable.unload_adapter = NULL;
    rp->vtable.active_adapter = NULL;
}

hu_provider_t hu_replay_provider_as_provider(hu_replay_provider_t *rp) {
    hu_provider_t p = {.ctx = rp, .vtable = rp && rp->inner.vtable ? &rp->vtable : NULL};
    return p;
}

void hu_replay_provider_reset_turn(hu_replay_provider_t *rp) {
    if (!rp)
        return;
    rp->calls = 0;
    rp->reply_calls = 0;
    rp->reply_fp = 0;
    rp->reply_bytes = 0;
    rp->reply_system_bytes = 0;
    free(rp->captured);
    rp->captured = NULL;
    rp->captured_len = 0;
}

void hu_replay_provider_deinit(hu_replay_provider_t *rp, hu_allocator_t *alloc) {
    if (!rp)
        return;
    free(rp->captured);
    rp->captured = NULL;
    rp->captured_len = 0;
    if (rp->owns_inner && rp->inner.vtable && rp->inner.vtable->deinit)
        rp->inner.vtable->deinit(rp->inner.ctx, alloc);
    rp->inner.vtable = NULL;
    rp->inner.ctx = NULL;
}

hu_error_t hu_replay_provider_create_local(hu_allocator_t *alloc, const char *provider_name,
                                           const char *endpoint, const char *api_key,
                                           hu_provider_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!alloc || !provider_name || !provider_name[0])
        return HU_ERR_INVALID_ARGUMENT;
    if (!hu_replay_url_is_loopback(endpoint))
        return HU_ERR_PERMISSION_DENIED;
    return hu_provider_create(alloc, provider_name, strlen(provider_name), api_key,
                              api_key ? strlen(api_key) : 0, endpoint, strlen(endpoint), out);
}
