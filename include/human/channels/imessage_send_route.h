#ifndef HU_CHANNELS_IMESSAGE_SEND_ROUTE_H
#define HU_CHANNELS_IMESSAGE_SEND_ROUTE_H

/*
 * Where an outbound iMessage-channel text goes, and how its outcome is told.
 *
 * WHY (2026-09-26): a 99-char reply to an RCS contact was generated and never
 * arrived. `imsg send --to <handle> --service auto` timed out (the CLI tries
 * iMessage first and falls back to SMS on its own clock), and the AppleScript
 * fallback addressed `buddy <handle> of (1st service whose service type =
 * iMessage)` — an empty iMessage placeholder that cannot reach an RCS/SMS
 * phone at all. Read-only probe of that contact's real chat
 * (`chat id "any;-;+1…"`) answers service type RCS; the iMessage form of the
 * same id does not exist (-1728). Nothing logged the final outcome, recorded
 * the loss, or told the owner.
 *
 * The fix is to answer a contact on the chat their message came in on:
 *   - every 1:1 inbound records (handle -> chat GUID, service);
 *   - a send to an SMS/RCS chat is addressed BY THAT CHAT
 *     (`imsg send --chat-guid`, AppleScript `send … to chat id "…"`), so
 *     Messages uses the chat's own service;
 *   - iMessage and unknown chats keep the exact argv/script they had before
 *     (byte-identical — the path that works is not touched);
 *   - every send attempt logs one aggregate line (no text, no handle).
 *
 * Pure C, no Apple dependencies: everything except the process spawn is
 * testable hermetically through hu_imsg_send_text_via() with a fake backend.
 * A shared iMessage-family leaf header (edge-context-isolation exemption).
 */

#include "human/channels/imessage_caps.h" /* hu_imessage_service_t */
#include <stdbool.h>
#include <stddef.h>

#define HU_IMSG_ROUTE_GUID_MAX 160

typedef struct hu_imsg_send_route {
    char chat_guid[HU_IMSG_ROUTE_GUID_MAX]; /* validated charset; "" = unknown */
    hu_imessage_service_t service;          /* service of the inbound message */
} hu_imsg_send_route_t;

#ifdef __cplusplus
extern "C" {
#endif

/* "iMessage" / "SMS" / "RCS" / "unknown". Never NULL. */
const char *hu_imsg_route_service_name(hu_imessage_service_t svc);

/* Remember the chat a 1:1 inbound from `handle` arrived on. `service` is
 * chat.db message.service; when NULL/unknown the GUID prefix ("SMS;", "RCS;",
 * "iMessage;") is used. A GUID outside [A-Za-z0-9;+@._-] is not stored — it
 * is interpolated into an executed script. Thread-safe; bounded table. */
void hu_imsg_route_note_inbound(const char *handle, size_t handle_len, const char *chat_guid,
                                const char *service);

/* The remembered route for `handle`. False when none is known, and then
 * `*out` is zeroed (service UNKNOWN, no GUID), so it is safe to read. */
bool hu_imsg_route_lookup(const char *handle, size_t handle_len, hu_imsg_send_route_t *out);

/* Forget every route (tests; and a channel restart). */
void hu_imsg_route_reset(void);

/* True when a send must be addressed by chat: a known GUID on SMS or RCS.
 * iMessage and unknown routes keep handle addressing. */
bool hu_imsg_route_by_chat(const hu_imsg_send_route_t *route);

/* imsg argv (NULL-terminated) into `argv[cap]`; returns argc, 0 on overflow.
 *   by-chat route: imsg send --chat-guid <guid> --text <text>
 *   otherwise:     imsg send --to <to> --text <text> --service <service>
 * Pointers alias the inputs and the route; keep them alive while argv is used. */
size_t hu_imsg_route_build_argv(const hu_imsg_send_route_t *route, const char *to, const char *text,
                                const char *service, const char **argv, size_t cap);

/* AppleScript send into `out`. `tgt_esc` / `msg_esc` are already escaped for
 * a double-quoted AppleScript string. Returns snprintf's count (<0 or >= cap
 * means it did not fit).
 *   by-chat route: send to `chat id "<guid>"` inside a 20 s timeout block
 *   otherwise:     the pre-existing buddy-of-service script, unchanged */
int hu_imsg_route_build_applescript(const hu_imsg_send_route_t *route, const char *as_service,
                                    const char *tgt_esc, const char *msg_esc, char *out,
                                    size_t cap);

/* imsg argv for one attachment (NULL-terminated); returns argc, 0 on overflow.
 *   by-chat route: imsg send --chat-guid <guid> --file <path>
 *   otherwise:     imsg send --to <to> --file <path> --service imessage (pre-fix) */
size_t hu_imsg_route_build_file_argv(const hu_imsg_send_route_t *route, const char *to,
                                     const char *path, const char **argv, size_t cap);

/* AppleScript sending one attachment to a by-chat route's chat; `path_esc` is
 * AppleScript-escaped. Returns snprintf's count; -1 when the route is not
 * chat-addressed (the caller keeps its pre-fix buddy script). */
int hu_imsg_route_build_chat_attach_script(const hu_imsg_send_route_t *route, const char *path_esc,
                                           char *out, size_t cap);

/* The aggregate outcome line, e.g.
 *   "[send] path=imsg result=fail chat_service=RCS by_chat=1"
 * `result` is "ok", "fail" or "landed" (reported failed, found in chat.db).
 * Counts/enums only: never text, never a handle. Returns the length. */
size_t hu_imsg_send_outcome_format(char *out, size_t cap, const char *path, const char *result,
                                   const hu_imsg_send_route_t *route);

/* ── Send orchestration over an injectable backend ────────────────────── */

typedef struct hu_imsg_send_backend {
    void *ctx;
    /* The imsg CLI is installed and enabled. */
    bool imsg_available;
    /* Run imsg; true only when the message is known to have gone out. */
    bool (*run_imsg)(void *ctx, const char *const *argv);
    /* Run one AppleScript under an outer process timeout; true on success. */
    bool (*run_applescript)(void *ctx, const char *script, unsigned timeout_s);
    /* One aggregate outcome line per attempt (production: hu_log_info). */
    void (*log_outcome)(void *ctx, const char *line);
    /* chat.db MAX(ROWID) of is_from_me rows for the route's chat (by-chat
     * routes, by GUID) or for the handle (its handle rows and its 1:1 chats).
     * 0 when there are none; -1 when chat.db cannot be read. NULL disables
     * landing checks. A failure report is not proof nothing went out: a
     * timed-out imsg or osascript may still deliver (Mindy, 2026-09-27). */
    int64_t (*sent_boundary)(void *ctx, const hu_imsg_send_route_t *route, const char *to);
    /* True when a plain, unerrored outbound row newer than `prior` carries
     * `text` (hu_imsg_landed_text_matches). A newer row alone is not proof:
     * Seth typing from his phone or a tapback lands in the same window. */
    bool (*landed)(void *ctx, const hu_imsg_send_route_t *route, const char *to, int64_t prior,
                   const char *text);
    void (*sleep_ms)(void *ctx, unsigned ms);
} hu_imsg_send_backend_t;

/* Does an outbound chat.db row's decoded text prove `sent` landed? Equal
 * after trimming surrounding whitespace. `row` NULL means the body could not
 * be decoded: the (already filtered) plain from-me row then counts, because a
 * duplicate to family is worse than a missed send. */
bool hu_imsg_landed_text_matches(const char *row, size_t row_len, const char *sent,
                                 size_t sent_len);

/* Pre-send boundary read retried once after this delay when chat.db is busy. */
#define HU_IMSG_BOUNDARY_RETRY_MS 200u

/* Landing-check polling after a failed attempt: every 500 ms, up to 5 s for a
 * chat-addressed (SMS/RCS) route, 2 s otherwise. */
#define HU_IMSG_LAND_POLL_MS         500u
#define HU_IMSG_LAND_POLLS_BY_CHAT   10u
#define HU_IMSG_LAND_POLLS_BY_HANDLE 4u
/* Outer osascript timeouts: the chat script carries its own 20 s Apple-event
 * timeout; the pre-fix buddy script relies on Messages' 120 s default. */
#define HU_IMSG_AS_TIMEOUT_BY_CHAT_S   30u
#define HU_IMSG_AS_TIMEOUT_BY_HANDLE_S 130u

typedef enum hu_imsg_send_path {
    HU_IMSG_SEND_PATH_NONE = 0,
    HU_IMSG_SEND_PATH_IMSG,
    HU_IMSG_SEND_PATH_APPLESCRIPT,
} hu_imsg_send_path_t;

typedef struct hu_imsg_send_request {
    const hu_imsg_send_route_t *route; /* NULL = no route known */
    const char *to;                    /* NUL-terminated handle */
    const char *text;                  /* NUL-terminated final text */
    const char *service;               /* imsg --service value */
    const char *as_service;            /* AppleScript service type */
    const char *tgt_esc;               /* AppleScript-escaped handle */
    const char *msg_esc;               /* AppleScript-escaped text */
} hu_imsg_send_request_t;

/* Try imsg (when available), then AppleScript. Logs one outcome line per
 * attempt. After a failed attempt, chat.db is polled for the text before
 * anything else happens: a send that landed is never sent again, and a final
 * failure is reported only when nothing landed (result=landed in the line).
 * When the boundary cannot be read (-1) landing cannot be checked and the
 * pre-fix behaviour applies: fall back to AppleScript.
 * Returns the path that delivered, or HU_IMSG_SEND_PATH_NONE. */
hu_imsg_send_path_t hu_imsg_send_text_via(const hu_imsg_send_backend_t *be,
                                          const hu_imsg_send_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* HU_CHANNELS_IMESSAGE_SEND_ROUTE_H */
