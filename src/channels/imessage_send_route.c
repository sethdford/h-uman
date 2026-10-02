/* Outbound route + outcome for iMessage-channel text sends. Contract and the
 * 2026-09-26 incident that motivated it: include/human/channels/imessage_send_route.h. */
#include "human/channels/imessage_send_route.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define ROUTE_SLOTS      64
#define ROUTE_HANDLE_MAX 128

typedef struct {
    char handle[ROUTE_HANDLE_MAX];
    size_t handle_len;
    hu_imsg_send_route_t route;
    unsigned long seq; /* recency, for replacement */
} route_slot_t;

static route_slot_t s_slots[ROUTE_SLOTS];
static unsigned long s_seq;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;

const char *hu_imsg_route_service_name(hu_imessage_service_t svc) {
    switch (svc) {
    case HU_IMSG_SERVICE_IMESSAGE:
        return "iMessage";
    case HU_IMSG_SERVICE_SMS:
        return "SMS";
    case HU_IMSG_SERVICE_RCS:
        return "RCS";
    default:
        return "unknown";
    }
}

/* Whitelist, not escaping: the GUID goes into argv and into an AppleScript
 * string literal. Real chat GUIDs ("any;-;+1555…", "SMS;-;x@y.com",
 * "iMessage;+;chat123") use only these characters. */
static bool guid_is_safe(const char *g, size_t len) {
    if (!g || len == 0 || len >= HU_IMSG_ROUTE_GUID_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        char ch = g[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == ';' || ch == '+' || ch == '@' || ch == '.' ||
                  ch == '_' || ch == '-';
        if (!ok)
            return false;
    }
    return true;
}

static hu_imessage_service_t service_from_guid_prefix(const char *g) {
    const char *semi = strchr(g, ';');
    if (!semi)
        return HU_IMSG_SERVICE_UNKNOWN;
    return hu_imessage_service_from_string(g, (size_t)(semi - g));
}

void hu_imsg_route_note_inbound(const char *handle, size_t handle_len, const char *chat_guid,
                                const char *service) {
    if (!handle || handle_len == 0 || handle_len >= ROUTE_HANDLE_MAX || !chat_guid)
        return;
    size_t glen = strlen(chat_guid);
    if (!guid_is_safe(chat_guid, glen))
        return;
    hu_imessage_service_t svc = service ? hu_imessage_service_from_string(service, strlen(service))
                                        : HU_IMSG_SERVICE_UNKNOWN;
    if (svc == HU_IMSG_SERVICE_UNKNOWN)
        svc = service_from_guid_prefix(chat_guid);

    pthread_mutex_lock(&s_mu);
    route_slot_t *slot = NULL;
    route_slot_t *oldest = &s_slots[0];
    for (size_t i = 0; i < ROUTE_SLOTS; i++) {
        route_slot_t *s = &s_slots[i];
        if (s->handle_len == handle_len && memcmp(s->handle, handle, handle_len) == 0) {
            slot = s;
            break;
        }
        if (s->seq < oldest->seq)
            oldest = s;
    }
    if (!slot) {
        slot = oldest;
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->handle, handle, handle_len);
        slot->handle_len = handle_len;
    }
    memcpy(slot->route.chat_guid, chat_guid, glen + 1);
    slot->route.service = svc;
    slot->seq = ++s_seq;
    pthread_mutex_unlock(&s_mu);
}

bool hu_imsg_route_lookup(const char *handle, size_t handle_len, hu_imsg_send_route_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out)); /* a miss must read as "no route", never garbage */
    if (!handle || handle_len == 0)
        return false;
    bool hit = false;
    pthread_mutex_lock(&s_mu);
    for (size_t i = 0; i < ROUTE_SLOTS; i++) {
        const route_slot_t *s = &s_slots[i];
        if (s->seq != 0 && s->handle_len == handle_len &&
            memcmp(s->handle, handle, handle_len) == 0) {
            *out = s->route;
            hit = true;
            break;
        }
    }
    pthread_mutex_unlock(&s_mu);
    return hit;
}

void hu_imsg_route_reset(void) {
    pthread_mutex_lock(&s_mu);
    memset(s_slots, 0, sizeof(s_slots));
    s_seq = 0;
    pthread_mutex_unlock(&s_mu);
}

bool hu_imsg_route_by_chat(const hu_imsg_send_route_t *route) {
    return route && route->chat_guid[0] &&
           (route->service == HU_IMSG_SERVICE_SMS || route->service == HU_IMSG_SERVICE_RCS);
}

size_t hu_imsg_route_build_argv(const hu_imsg_send_route_t *route, const char *to, const char *text,
                                const char *service, const char **argv, size_t cap) {
    if (!argv || !text)
        return 0;
    if (hu_imsg_route_by_chat(route)) {
        if (cap < 7)
            return 0;
        /* No --service: the chat carries its own (SMS or RCS). */
        const char *v[] = {"imsg", "send", "--chat-guid", route->chat_guid, "--text", text, NULL};
        memcpy(argv, v, sizeof(v));
        return 6;
    }
    if (cap < 9 || !to || !service)
        return 0;
    /* Pre-fix argv, unchanged. */
    const char *v[] = {"imsg", "send", "--to", to, "--text", text, "--service", service, NULL};
    memcpy(argv, v, sizeof(v));
    return 8;
}

int hu_imsg_route_build_applescript(const hu_imsg_send_route_t *route, const char *as_service,
                                    const char *tgt_esc, const char *msg_esc, char *out,
                                    size_t cap) {
    if (!out || cap == 0 || !msg_esc)
        return -1;
    if (hu_imsg_route_by_chat(route)) {
        /* The chat's own account decides the service. Bounded: an unanswered
         * Apple event otherwise holds the daemon for its 120 s default. */
        return snprintf(out, cap,
                        "tell application \"Messages\"\n"
                        "  with timeout of 20 seconds\n"
                        "    set targetChat to chat id \"%s\"\n"
                        "    send \"%s\" to targetChat\n"
                        "  end timeout\n"
                        "end tell",
                        route->chat_guid, msg_esc);
    }
    if (!as_service || !tgt_esc)
        return -1;
    /* Pre-fix script, unchanged. */
    return snprintf(out, cap,
                    "tell application \"Messages\"\n"
                    "  set targetService to 1st service whose service type = %s\n"
                    "  set targetBuddy to buddy \"%s\" of targetService\n"
                    "  send \"%s\" to targetBuddy\n"
                    "end tell",
                    as_service, tgt_esc, msg_esc);
}

size_t hu_imsg_route_build_file_argv(const hu_imsg_send_route_t *route, const char *to,
                                     const char *path, const char **argv, size_t cap) {
    if (!argv || !path)
        return 0;
    if (hu_imsg_route_by_chat(route)) {
        if (cap < 7)
            return 0;
        const char *v[] = {"imsg", "send", "--chat-guid", route->chat_guid, "--file", path, NULL};
        memcpy(argv, v, sizeof(v));
        return 6;
    }
    if (cap < 9 || !to)
        return 0;
    /* Pre-fix attachment argv, unchanged. */
    const char *v[] = {"imsg", "send", "--to", to, "--file", path, "--service", "imessage", NULL};
    memcpy(argv, v, sizeof(v));
    return 8;
}

int hu_imsg_route_build_chat_attach_script(const hu_imsg_send_route_t *route, const char *path_esc,
                                           char *out, size_t cap) {
    if (!hu_imsg_route_by_chat(route) || !path_esc || !out || cap == 0)
        return -1;
    return snprintf(out, cap,
                    "tell application \"Messages\"\n"
                    "  with timeout of 20 seconds\n"
                    "    set targetChat to chat id \"%s\"\n"
                    "    send (POSIX file \"%s\") to targetChat\n"
                    "  end timeout\n"
                    "end tell",
                    route->chat_guid, path_esc);
}

size_t hu_imsg_send_outcome_format(char *out, size_t cap, const char *path, const char *result,
                                   const hu_imsg_send_route_t *route) {
    if (!out || cap == 0)
        return 0;
    int n = snprintf(out, cap, "[send] path=%s result=%s chat_service=%s by_chat=%d",
                     path ? path : "?", result ? result : "?",
                     hu_imsg_route_service_name(route ? route->service : HU_IMSG_SERVICE_UNKNOWN),
                     hu_imsg_route_by_chat(route) ? 1 : 0);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

static void log_attempt(const hu_imsg_send_backend_t *be, const char *path, const char *result,
                        const hu_imsg_send_route_t *route) {
    if (!be->log_outcome)
        return;
    char line[128];
    hu_imsg_send_outcome_format(line, sizeof(line), path, result, route);
    be->log_outcome(be->ctx, line);
}

/* Did an is_from_me row newer than `prior` appear? Polls a bounded number of
 * times. False when the boundary is unknown — no evidence is not delivery. */
static bool landed_since(const hu_imsg_send_backend_t *be, const hu_imsg_send_request_t *req,
                         int64_t prior) {
    if (prior < 0 || !be->landed)
        return false;
    unsigned polls = hu_imsg_route_by_chat(req->route) ? HU_IMSG_LAND_POLLS_BY_CHAT
                                                       : HU_IMSG_LAND_POLLS_BY_HANDLE;
    for (unsigned i = 0; i < polls; i++) {
        if (be->sleep_ms)
            be->sleep_ms(be->ctx, HU_IMSG_LAND_POLL_MS);
        if (be->landed(be->ctx, req->route, req->to, prior, req->text))
            return true;
    }
    return false;
}

hu_imsg_send_path_t hu_imsg_send_text_via(const hu_imsg_send_backend_t *be,
                                          const hu_imsg_send_request_t *req) {
    if (!be || !req || !req->text)
        return HU_IMSG_SEND_PATH_NONE;
    /* Read before any attempt: 0 = chat has no outbound rows yet, -1 = unknown. */
    int64_t prior = be->sent_boundary ? be->sent_boundary(be->ctx, req->route, req->to) : -1;
    if (prior < 0 && be->sent_boundary) { /* chat.db busy: one retry */
        if (be->sleep_ms)
            be->sleep_ms(be->ctx, HU_IMSG_BOUNDARY_RETRY_MS);
        prior = be->sent_boundary(be->ctx, req->route, req->to);
    }
    bool imsg_tried = false;
    if (be->imsg_available && be->run_imsg) {
        const char *argv[10];
        if (hu_imsg_route_build_argv(req->route, req->to, req->text, req->service, argv, 10) > 0) {
            imsg_tried = true;
            if (be->run_imsg(be->ctx, argv)) {
                log_attempt(be, "imsg", "ok", req->route);
                return HU_IMSG_SEND_PATH_IMSG;
            }
            /* A timed-out imsg may have delivered: never send it twice. */
            if (landed_since(be, req, prior)) {
                log_attempt(be, "imsg", "landed", req->route);
                return HU_IMSG_SEND_PATH_IMSG;
            }
            log_attempt(be, "imsg", "fail", req->route);
        }
    }
    if (!be->run_applescript)
        return HU_IMSG_SEND_PATH_NONE;
    /* imsg failed and chat.db cannot tell whether it delivered anyway: a
     * second send could double-text family. A missed send is less bad —
     * report the failure (recorded, owner told "may not have been delivered"). */
    if (imsg_tried && prior < 0) {
        log_attempt(be, "applescript", "skipped", req->route);
        return HU_IMSG_SEND_PATH_NONE;
    }
    size_t cap = 256 + strlen(req->msg_esc ? req->msg_esc : "") +
                 strlen(req->tgt_esc ? req->tgt_esc : "") + HU_IMSG_ROUTE_GUID_MAX;
    /* The caller caps text at 1000 chars (<= 2000 escaped), so this always
     * fits; an oversized request is refused and logged, never truncated. */
    char script[4608];
    if (cap > sizeof(script)) {
        log_attempt(be, "applescript", "fail", req->route);
        return HU_IMSG_SEND_PATH_NONE;
    }
    int n = hu_imsg_route_build_applescript(req->route, req->as_service, req->tgt_esc, req->msg_esc,
                                            script, cap);
    if (n < 0 || (size_t)n >= cap) {
        log_attempt(be, "applescript", "fail", req->route);
        return HU_IMSG_SEND_PATH_NONE;
    }
    unsigned timeout_s = hu_imsg_route_by_chat(req->route) ? HU_IMSG_AS_TIMEOUT_BY_CHAT_S
                                                           : HU_IMSG_AS_TIMEOUT_BY_HANDLE_S;
    if (be->run_applescript(be->ctx, script, timeout_s)) {
        log_attempt(be, "applescript", "ok", req->route);
        return HU_IMSG_SEND_PATH_APPLESCRIPT;
    }
    /* An expired Apple-event timeout can still deliver: look before calling
     * it a failure. */
    if (landed_since(be, req, prior)) {
        log_attempt(be, "applescript", "landed", req->route);
        return HU_IMSG_SEND_PATH_APPLESCRIPT;
    }
    log_attempt(be, "applescript", "fail", req->route);
    return HU_IMSG_SEND_PATH_NONE;
}

static void trim_span(const char **p, size_t *len) {
    while (*len > 0 && ((*p)[0] == ' ' || (*p)[0] == '\n' || (*p)[0] == '\t' || (*p)[0] == '\r')) {
        (*p)++;
        (*len)--;
    }
    while (*len > 0 && ((*p)[*len - 1] == ' ' || (*p)[*len - 1] == '\n' || (*p)[*len - 1] == '\t' ||
                        (*p)[*len - 1] == '\r'))
        (*len)--;
}

bool hu_imsg_landed_text_matches(const char *row, size_t row_len, const char *sent,
                                 size_t sent_len) {
    if (!row)
        return true; /* undecodable plain from-me row: the duplicate is worse */
    if (!sent)
        return false;
    trim_span(&row, &row_len);
    trim_span(&sent, &sent_len);
    return row_len > 0 && row_len == sent_len && memcmp(row, sent, row_len) == 0;
}
