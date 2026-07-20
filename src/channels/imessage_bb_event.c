/* Parser + activation gate for `imsg watch --bb-events` IMCore bridge events.
 *
 * Schema captured empirically 2026-07-19 (SIP disabled, bridge live) —
 * see docs/plans/2026-07-19-native-imessage/bb-events-schema.md for the
 * verbatim stdout lines this parser is written against. */

#include "human/channels/imessage_bb_event.h"

#include "human/core/json.h"

#include <string.h>

/* Bounded copy into a fixed field; always NUL-terminates, never truncates
 * mid-write into an unterminated buffer. */
static void bb_copy_id(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Length-bounded substring scan. NOT memmem(): that is a BSD extension present
 * on macOS but gated behind _GNU_SOURCE in glibc, so using it here would build
 * clean locally and break the Linux CI arm. The input is a pipe chunk and is
 * not NUL-terminated, so str* functions are equally unavailable. */
static bool bb_contains(const char *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || hay_len < n)
        return false;
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0)
            return true;
    }
    return false;
}

static hu_imessage_bb_kind_t bb_kind_from_event_name(const char *event) {
    if (!event)
        return HU_IMSG_BB_UNKNOWN;
    /* Exact match, not substring: "stopped-typing" contains "typing", and a
     * substring test would collapse start/stop into one bucket — the exact
     * failure mode in ~/.claude/rules/substring-classifier-pitfalls.md. */
    if (strcmp(event, "started-typing") == 0)
        return HU_IMSG_BB_TYPING_START;
    if (strcmp(event, "stopped-typing") == 0)
        return HU_IMSG_BB_TYPING_STOP;
    if (strcmp(event, "aliases-removed") == 0)
        return HU_IMSG_BB_ALIASES_REMOVED;
    return HU_IMSG_BB_UNKNOWN;
}

bool hu_imessage_bb_event_parse(hu_allocator_t *alloc, const char *line, size_t len,
                                hu_imessage_bb_event_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!alloc || !line || len == 0)
        return false;

    /* Cheap prefilter: the overwhelming majority of lines on this stream are
     * chat.db message rows. Skip the JSON parse unless the discriminator is
     * present at all. Correctness still comes from the keyed check below —
     * this is only a fast reject. */
    if (!bb_contains(line, len, "bridge-event"))
        return false;

    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, line, len, &root) != HU_OK || !root)
        return false;

    bool is_bridge = false;
    const char *kind = hu_json_get_string(root, "kind");
    if (kind && strcmp(kind, "bridge-event") == 0)
        is_bridge = true;

    if (is_bridge) {
        out->kind = bb_kind_from_event_name(hu_json_get_string(root, "event"));

        /* `data` may legitimately be absent ({} when the dylib record had no
         * payload). Parse what is there; do not drop the event. */
        hu_json_value_t *data = hu_json_object_get(root, "data");
        if (data) {
            /* Observed key is camelCase `chatGuid`; the chat.db message rows on
             * the same stream use snake_case `chat_guid`. Accept both so a
             * bridge revision that switches convention does not blind us. */
            const char *chat = hu_json_get_string(data, "chatGuid");
            if (!chat)
                chat = hu_json_get_string(data, "chat_guid");
            bb_copy_id(out->chat_guid, sizeof(out->chat_guid), chat);
            bb_copy_id(out->handle, sizeof(out->handle), hu_json_get_string(data, "handle"));
            out->timestamp = hu_json_get_number(data, "timestamp", 0.0);
        }
    }

    hu_json_free(alloc, root);
    if (!is_bridge)
        memset(out, 0, sizeof(*out));
    return is_bridge;
}

void hu_imessage_bb_stream_consume(hu_imessage_bb_stream_t *st, hu_allocator_t *alloc,
                                   const char *buf, size_t n, hu_imessage_bb_event_fn on_event,
                                   void *user) {
    if (!st || !alloc || !buf)
        return;
    for (size_t i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch != '\n') {
            /* An over-long line is dropped to the next newline rather than
             * truncated: a truncated line would be half-valid JSON and could
             * parse into a plausible-but-wrong event. */
            if (st->line_len < sizeof(st->line))
                st->line[st->line_len++] = ch;
            else
                st->overflow = true;
            continue;
        }
        /* Tolerate CRLF: strip a trailing \r before parsing. */
        size_t len = st->line_len;
        if (len > 0 && st->line[len - 1] == '\r')
            len--;

        if (!st->overflow && len > 0) {
            hu_imessage_bb_event_t ev;
            if (hu_imessage_bb_event_parse(alloc, st->line, len, &ev) && on_event)
                on_event(&ev, user);
        }
        st->line_len = 0;
        st->overflow = false;
    }
}

hu_imessage_bb_mode_t hu_imessage_bb_mode_from_env(const char *value) {
    if (!value || !*value)
        return HU_IMSG_BB_MODE_OFF;
    /* LIVE > SHADOW > OFF. Exact matches only — fail closed on anything else
     * so a typo degrades to OFF rather than silently arming the subsystem. */
    if (strcmp(value, "live") == 0)
        return HU_IMSG_BB_MODE_LIVE;
    if (strcmp(value, "shadow") == 0)
        return HU_IMSG_BB_MODE_SHADOW;
    return HU_IMSG_BB_MODE_OFF;
}

bool hu_imessage_bb_should_hold_send(hu_imessage_bb_kind_t last_kind, double last_event_ts,
                                     double now_ts, double *hold_seconds_out) {
    if (hold_seconds_out)
        *hold_seconds_out = 0.0;

    /* Only a typing-START can justify holding: any other state (no event seen,
     * an explicit stop, an alias change) means we have no evidence the contact
     * is mid-sentence, and absence of evidence must not hold a send. */
    if (last_kind != HU_IMSG_BB_TYPING_START)
        return false;

    /* Clock skew / replayed event: a "start" timestamped in the future is not
     * trustworthy evidence. Fail closed (do not hold). */
    double elapsed = now_ts - last_event_ts;
    if (elapsed < 0.0)
        return false;

    return hu_imessage_bb_hold_policy(elapsed, hold_seconds_out);
}

/* How long a single typing-START may suppress a send.
 *
 * Anchored on Apple's own behavior rather than a taste guess: the iMessage
 * typing indicator is client-side expiring — it disappears roughly 60s after
 * typing begins, even if the contact is still typing. Past that point the
 * SENDER's own device no longer advertises typing, so continuing to hold means
 * waiting on a signal Apple itself has retired. It is also where the
 * abandoned-draft case lands: iMessage emits no stop event when someone simply
 * puts the phone down, so without a ceiling a single START would suppress
 * replies forever.
 *
 * 60s also covers the composition-time distribution: survey data puts ~47% of
 * texts under 30s to write and only ~10% over 60s. Holding to 60s therefore
 * avoids interrupting ~90% of in-progress messages while never stalling a
 * reply longer than Apple's own indicator would have been visible. */
#define HU_IMSG_BB_TYPING_STALE_S 60.0
/* Re-check granularity while holding: bounded so the caller re-evaluates
 * against fresh events (a stop may land at any moment) instead of committing
 * to one long sleep. */
#define HU_IMSG_BB_HOLD_RECHECK_S 2.0

bool hu_imessage_bb_hold_policy(double elapsed_s, double *hold_seconds_out) {
    if (hold_seconds_out)
        *hold_seconds_out = 0.0;

    /* Stale or abandoned draft: stop holding. */
    if (elapsed_s >= HU_IMSG_BB_TYPING_STALE_S)
        return false;

    if (hold_seconds_out) {
        double remaining = HU_IMSG_BB_TYPING_STALE_S - elapsed_s;
        *hold_seconds_out =
            remaining < HU_IMSG_BB_HOLD_RECHECK_S ? remaining : HU_IMSG_BB_HOLD_RECHECK_S;
    }
    return true;
}
