/* src/channels/matrix_reactions.c
 *
 * Phase 2 of docs/plans/2026-05-18-imessage-sota.md: cross-channel
 * reaction emit path for Matrix m.reaction events delivered via the
 * /sync long-polling endpoint. Mirrors src/channels/slack_reactions.c
 * / discord_reactions.c / telegram_reactions.c.
 *
 * Split out from src/channels/matrix.c on purpose: the
 * human/channels/reaction_event.h enumerator names
 * HU_REACTION_KIND_QUESTION and HU_REACTION_KIND_CUSTOM_EMOJI collide
 * with the older hu_reaction_type_t enum declared in human/channel.h
 * (which matrix.c needs for its outbound m.reaction send path).
 * Pulling both headers into one TU is a compile error. matrix.c
 * includes human/channel.h and cannot also include reaction_event.h,
 * so this branch lives in a TU that only includes reaction_event.h.
 *
 * Spec reference: https://spec.matrix.org/v1.11/client-server-api/#mreaction
 *
 *   { "type":"m.reaction",
 *     "sender":"@alice:example.com",
 *     "origin_server_ts":1730000000000,
 *     "event_id":"$abc...",
 *     "content":{ "m.relates_to":{
 *         "rel_type":"m.annotation",
 *         "event_id":"$target_event_id",
 *         "key":"🔥" }}}
 *
 * Phase-1 scope cut: Matrix REMOVE reactions are delivered as
 * `m.room.redaction` events that reference an earlier `m.reaction`
 * event_id. The redaction flow requires keeping a map from
 * reaction_event_id -> (target, key) so the redaction can be turned
 * into a removal event. We DO NOT implement that here; only ADD
 * events are emitted. is_removal is always 0 on the produced
 * events. Documented as a known limitation.
 *
 * Two symbols are exposed (no header — matrix.c uses an extern decl
 * at function scope, the test forward-declares directly):
 *
 *   1. hu_matrix_handle_reaction_sync_event(event, bot_user_id)
 *      — invoked from matrix.c's /sync timeline-events loop BEFORE the
 *      existing type=="m.room.message" branch. Takes the already-parsed
 *      JSON event object (matrix.c parses the whole /sync response
 *      once; re-parsing per event would be wasteful). Returns 1 if the
 *      event was an m.reaction (caller should skip its message branch
 *      for this event), 0 otherwise.
 *      NOTE: takes the parsed event AND a room_id since matrix's /sync
 *      response groups events by room — the event JSON itself does not
 *      carry the room_id in m.reaction. (matrix.c already has room_id
 *      in scope when walking timeline events.)
 *
 *   2. hu_matrix_handle_reaction_event_for_test (HU_IS_TEST only) —
 *      same parse + filter + normalize pipeline but with a STRICTER
 *      return contract: filter rejections come back as
 *      HU_ERR_NOT_SUPPORTED so unit tests can assert on the filter
 *      behavior. Accepts a full JSON body (the whole event object) for
 *      ergonomics; the test extracts room_id from a top-level
 *      `room_id` field if present, since the spec doesn't put it on
 *      the event itself but our test fixtures synthesize it for
 *      assertion convenience. */

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Matrix m.reaction `key` is a unicode glyph in standard practice; for
 * custom reactions some clients put a shortcode or MXC URI but the
 * spec is open. We treat anything not matching our standard table as
 * CUSTOM_EMOJI with the key preserved on evt.emoji. Same coarse
 * substring approach as discord/whatsapp — picks up variation
 * selectors and skin-tone modifiers. */
static int matrix_normalize_emoji(const char *key, hu_reaction_kind_t *out_kind,
                                  hu_reaction_polarity_t *out_polarity) {
    if (!key || !*key || !out_kind || !out_polarity)
        return 1;
    *out_kind = HU_REACTION_UNKNOWN;
    *out_polarity = HU_REACTION_NEUTRAL;

    /* Heart family — LOVE. */
    if (strstr(key, "\xe2\x9d\xa4") /* ❤ */ || strstr(key, "\xf0\x9f\x92\x95") /* 💕 */ ||
        strstr(key, "\xf0\x9f\x92\x96") /* 💖 */ || strstr(key, "\xf0\x9f\x92\x98") /* 💘 */) {
        *out_kind = HU_REACTION_LOVE;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Thumbs up — LIKE. */
    if (strstr(key, "\xf0\x9f\x91\x8d") /* 👍 */) {
        *out_kind = HU_REACTION_LIKE;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Thumbs down — DISLIKE. */
    if (strstr(key, "\xf0\x9f\x91\x8e") /* 👎 */) {
        *out_kind = HU_REACTION_DISLIKE;
        *out_polarity = HU_REACTION_NEGATIVE;
        return 0;
    }
    /* Laughter family — LAUGH. */
    if (strstr(key, "\xf0\x9f\x98\x82") /* 😂 */ || strstr(key, "\xf0\x9f\xa4\xa3") /* 🤣 */ ||
        strstr(key, "\xf0\x9f\x98\x86") /* 😆 */) {
        *out_kind = HU_REACTION_LAUGH;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Exclamation — EMPHASIZE. */
    if (strstr(key, "\xe2\x80\xbc") /* ‼ */ || strstr(key, "\xe2\x9d\x97") /* ❗ */) {
        *out_kind = HU_REACTION_EMPHASIZE;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Question marks — QUESTION (neutral). */
    if (strstr(key, "\xe2\x9d\x93") /* ❓ */ || strstr(key, "\xe2\x9d\x94") /* ❔ */ ||
        strstr(key, "\xf0\x9f\xa4\x94") /* 🤔 */) {
        *out_kind = HU_REACTION_KIND_QUESTION;
        *out_polarity = HU_REACTION_NEUTRAL;
        return 0;
    }
    *out_kind = HU_REACTION_KIND_CUSTOM_EMOJI;
    *out_polarity = HU_REACTION_POSITIVE;
    return 0;
}

/* Core dispatch: extract from a parsed m.reaction event and emit. The
 * room_id is passed separately because the spec puts it on the /sync
 * envelope, not the event itself. Returns 0 on success, non-zero on
 * malformed / filter rejection. */
static int matrix_dispatch_reaction(const hu_json_value_t *event, const char *room_id,
                                    const char *bot_user_id) {
    if (!event || event->type != HU_JSON_OBJECT)
        return 1;

    const char *ev_type = hu_json_get_string(event, "type");
    if (!ev_type || strcmp(ev_type, "m.reaction") != 0)
        return 1;

    const char *sender = hu_json_get_string(event, "sender");
    double ts_ms = hu_json_get_number(event, "origin_server_ts", 0.0);

    hu_json_value_t *content = hu_json_object_get(event, "content");
    if (!content || content->type != HU_JSON_OBJECT)
        return 1;
    hu_json_value_t *relates = hu_json_object_get(content, "m.relates_to");
    if (!relates || relates->type != HU_JSON_OBJECT)
        return 1;
    const char *rel_type = hu_json_get_string(relates, "rel_type");
    if (!rel_type || strcmp(rel_type, "m.annotation") != 0)
        return 1;
    const char *target_event_id = hu_json_get_string(relates, "event_id");
    const char *key = hu_json_get_string(relates, "key");
    if (!target_event_id || !key)
        return 1;

    /* Filter: skip self-reactions. */
    if (sender && bot_user_id && strcmp(sender, bot_user_id) == 0)
        return 1;

    hu_reaction_kind_t k;
    hu_reaction_polarity_t p;
    if (matrix_normalize_emoji(key, &k, &p) != 0)
        return 1;

    hu_reaction_event_t evt = {0};
    evt.channel_id = "matrix";
    evt.target_thread_id = room_id ? strdup(room_id) : NULL;
    evt.target_message_ref = strdup(target_event_id);
    evt.sender_handle = sender ? strdup(sender) : NULL;
    evt.kind = k;
    evt.polarity = p;
    /* Phase-1 scope: no redaction handling, so all events are ADD. */
    evt.is_removal = 0;
    evt.timestamp_unix = (int64_t)ts_ms / 1000;
    evt.emoji = (k == HU_REACTION_KIND_CUSTOM_EMOJI) ? strdup(key) : NULL;

    hu_reaction_handler_handle_event(&evt);

    free((void *)evt.target_thread_id);
    free((void *)evt.target_message_ref);
    free((void *)evt.sender_handle);
    free((void *)evt.emoji);
    return 0;
}

int hu_matrix_handle_reaction_sync_event(const hu_json_value_t *event, const char *room_id,
                                         const char *bot_user_id) {
    if (!event || event->type != HU_JSON_OBJECT)
        return 0;
    const char *ev_type = hu_json_get_string(event, "type");
    if (!ev_type || strcmp(ev_type, "m.reaction") != 0)
        return 0;
    /* Dispatch return code is informational; once we recognize the
     * event type we own it (return 1) so caller skips its message
     * branch. */
    (void)matrix_dispatch_reaction(event, room_id, bot_user_id);
    return 1;
}

#if HU_IS_TEST
hu_error_t hu_matrix_handle_reaction_event_for_test(const char *body, size_t body_len,
                                                    hu_allocator_t *alloc, const char *bot_user_id,
                                                    hu_reaction_event_t *out) {
    if (!body || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t perr = hu_json_parse(alloc, body, body_len ? body_len : strlen(body), &root);
    if (perr != HU_OK || !root)
        return HU_ERR_INVALID_ARGUMENT;

    const char *ev_type = hu_json_get_string(root, "type");
    if (!ev_type || strcmp(ev_type, "m.reaction") != 0) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *sender = hu_json_get_string(root, "sender");
    /* Test fixtures embed `room_id` at the top level for assertion
     * convenience; the real /sync envelope groups events by room and
     * matrix.c passes the room_id in. */
    const char *room_id = hu_json_get_string(root, "room_id");
    double ts_ms = hu_json_get_number(root, "origin_server_ts", 0.0);

    hu_json_value_t *content = hu_json_object_get(root, "content");
    if (!content || content->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_json_value_t *relates = hu_json_object_get(content, "m.relates_to");
    if (!relates || relates->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *rel_type = hu_json_get_string(relates, "rel_type");
    if (!rel_type || strcmp(rel_type, "m.annotation") != 0) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *target_event_id = hu_json_get_string(relates, "event_id");
    const char *key = hu_json_get_string(relates, "key");
    if (!target_event_id || !key) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* TEST CONTRACT: filter rejections return HU_ERR_NOT_SUPPORTED. */
    if (sender && bot_user_id && strcmp(sender, bot_user_id) == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }

    hu_reaction_kind_t k;
    hu_reaction_polarity_t p;
    if (matrix_normalize_emoji(key, &k, &p) != 0) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    out->channel_id = "matrix";
    out->target_thread_id = room_id ? strdup(room_id) : NULL;
    out->target_message_ref = strdup(target_event_id);
    out->sender_handle = sender ? strdup(sender) : NULL;
    out->kind = k;
    out->polarity = p;
    out->is_removal = 0;
    out->timestamp_unix = (int64_t)ts_ms / 1000;
    out->emoji = (k == HU_REACTION_KIND_CUSTOM_EMOJI) ? strdup(key) : NULL;

    hu_json_free(alloc, root);
    return HU_OK;
}
#endif
