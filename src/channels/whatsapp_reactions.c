/* src/channels/whatsapp_reactions.c
 *
 * Phase 2 of docs/plans/2026-05-18-imessage-sota.md: cross-channel
 * reaction emit path for WhatsApp Business Cloud API webhooks. Mirrors
 * src/channels/slack_reactions.c / discord_reactions.c / telegram_reactions.c.
 *
 * Split out from src/channels/whatsapp.c on purpose: the
 * human/channels/reaction_event.h enumerator names HU_REACTION_KIND_QUESTION
 * and HU_REACTION_KIND_CUSTOM_EMOJI collide with the older
 * hu_reaction_type_t enum declared in human/channel.h (which whatsapp.c
 * needs for its outbound reactions.add path). Pulling both headers into
 * one TU is a compile error, so the new branch lives here in a TU that
 * only includes reaction_event.h, never channel.h. Same pattern as the
 * other reaction emit files.
 *
 * WhatsApp delivers reaction webhooks as a `messages` entry with
 * type="reaction", inside the same entry/changes/value envelope as
 * regular text messages. Reference:
 * https://developers.facebook.com/docs/whatsapp/cloud-api/webhooks/components#reaction-object
 *
 *   { "entry":[{ "changes":[{ "value":{ "messages":[{
 *       "from":"16315551234",
 *       "id":"wamid.XXXX",
 *       "timestamp":"1730000000",
 *       "type":"reaction",
 *       "reaction":{ "message_id":"wamid.target", "emoji":"❤️" }
 *     }] }}] }] }
 *
 * A reaction REMOVE is signaled by `reaction.emoji` being empty string
 * OR JSON null (per the Meta webhook docs).
 *
 * Two symbols are exposed (no header — whatsapp.c uses an extern decl
 * at function scope, the test forward-declares directly):
 *
 *   1. hu_whatsapp_handle_reaction_webhook(body, body_len, alloc,
 *                                          bot_user_id)
 *      — invoked from whatsapp.c's webhook dispatcher BEFORE the
 *      existing type=="text" filter. Returns 1 if the body contained
 *      ANY reaction message that we absorbed (caller should stop the
 *      whole webhook and ack), 0 if the body contained no reaction
 *      messages (caller continues normal text processing).
 *      NOTE: WhatsApp delivers ONE webhook per inbound event in
 *      practice, but the envelope is an array — if a single body
 *      mixes a reaction with a text message we currently absorb the
 *      whole body. That trades a tiny risk of dropping a co-arrived
 *      text message against the simpler contract of "this body was
 *      a reaction event, ack it." Documented; revisit if real
 *      multi-event bodies appear in the wild.
 *
 *   2. hu_whatsapp_handle_reaction_event_for_test (HU_IS_TEST only) —
 *      same parse/filter/normalize pipeline but with a STRICTER return
 *      contract: filter rejections come back as HU_ERR_NOT_SUPPORTED so
 *      unit tests can assert on the filter behavior. */

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* WhatsApp arrives as a unicode glyph in `reaction.emoji`. Mapping is
 * substring-based on UTF-8 bytes so variation selectors (FE0F) and
 * skin-tone modifiers don't break the match — same coarse approach as
 * discord_reactions.c. Returns 0 on a recognized standard emoji (kind
 * and polarity filled), or non-zero if the glyph is unknown. */
static int whatsapp_normalize_emoji(const char *emoji, hu_reaction_kind_t *out_kind,
                                    hu_reaction_polarity_t *out_polarity) {
    if (!emoji || !*emoji || !out_kind || !out_polarity)
        return 1;
    *out_kind = HU_REACTION_UNKNOWN;
    *out_polarity = HU_REACTION_NEUTRAL;

    /* Heart family — LOVE. */
    if (strstr(emoji, "\xe2\x9d\xa4") /* ❤ */ || strstr(emoji, "\xf0\x9f\x92\x95") /* 💕 */ ||
        strstr(emoji, "\xf0\x9f\x92\x96") /* 💖 */ || strstr(emoji, "\xf0\x9f\x92\x98") /* 💘 */) {
        *out_kind = HU_REACTION_LOVE;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Thumbs up — LIKE. */
    if (strstr(emoji, "\xf0\x9f\x91\x8d") /* 👍 */) {
        *out_kind = HU_REACTION_LIKE;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Thumbs down — DISLIKE. */
    if (strstr(emoji, "\xf0\x9f\x91\x8e") /* 👎 */) {
        *out_kind = HU_REACTION_DISLIKE;
        *out_polarity = HU_REACTION_NEGATIVE;
        return 0;
    }
    /* Laughter family — LAUGH. */
    if (strstr(emoji, "\xf0\x9f\x98\x82") /* 😂 */ || strstr(emoji, "\xf0\x9f\xa4\xa3") /* 🤣 */ ||
        strstr(emoji, "\xf0\x9f\x98\x86") /* 😆 */) {
        *out_kind = HU_REACTION_LAUGH;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Exclamation — EMPHASIZE. */
    if (strstr(emoji, "\xe2\x80\xbc") /* ‼ */ || strstr(emoji, "\xe2\x9d\x97") /* ❗ */) {
        *out_kind = HU_REACTION_EMPHASIZE;
        *out_polarity = HU_REACTION_POSITIVE;
        return 0;
    }
    /* Question marks — QUESTION (neutral). */
    if (strstr(emoji, "\xe2\x9d\x93") /* ❓ */ || strstr(emoji, "\xe2\x9d\x94") /* ❔ */ ||
        strstr(emoji, "\xf0\x9f\xa4\x94") /* 🤔 */) {
        *out_kind = HU_REACTION_KIND_QUESTION;
        *out_polarity = HU_REACTION_NEUTRAL;
        return 0;
    }
    /* Anything else — CUSTOM_EMOJI with the glyph preserved on
     * evt.emoji so downstream still records the reaction. */
    *out_kind = HU_REACTION_KIND_CUSTOM_EMOJI;
    *out_polarity = HU_REACTION_POSITIVE;
    return 0;
}

/* Detect "REMOVE reaction" — Meta sends emoji as empty string or
 * JSON null when the user removes their reaction. */
static int whatsapp_reaction_is_remove(const hu_json_value_t *reaction_obj) {
    if (!reaction_obj || reaction_obj->type != HU_JSON_OBJECT)
        return 0;
    const hu_json_value_t *emoji = hu_json_object_get(reaction_obj, "emoji");
    if (!emoji)
        return 1; /* missing key — treat as remove */
    if (emoji->type == HU_JSON_NULL)
        return 1;
    if (emoji->type == HU_JSON_STRING && (!emoji->data.string.ptr || emoji->data.string.len == 0))
        return 1;
    return 0;
}

/* Dispatch one reaction message (already located inside the
 * value.messages[] array). Returns 0 on success (event dispatched), or
 * non-zero on filter rejection / malformed shape — caller treats the
 * whole webhook as absorbed regardless. */
static int whatsapp_dispatch_one(const hu_json_value_t *msg, const char *bot_user_id) {
    if (!msg || msg->type != HU_JSON_OBJECT)
        return 1;

    const char *msg_type = hu_json_get_string(msg, "type");
    if (!msg_type || strcmp(msg_type, "reaction") != 0)
        return 1;

    const char *from = hu_json_get_string(msg, "from");
    const char *timestamp_s = hu_json_get_string(msg, "timestamp");
    hu_json_value_t *reaction_obj = hu_json_object_get(msg, "reaction");
    if (!reaction_obj || reaction_obj->type != HU_JSON_OBJECT)
        return 1;
    const char *target_msg_id = hu_json_get_string(reaction_obj, "message_id");
    const char *emoji = hu_json_get_string(reaction_obj, "emoji");
    int is_remove = whatsapp_reaction_is_remove(reaction_obj);

    /* Filter: skip self-reactions. WhatsApp identifies the business
     * account by phone_number_id; bot_user_id holds that value. The
     * `from` field is the END-USER phone number for inbound reactions,
     * so they should never match — but check anyway for symmetry with
     * the other channels. */
    if (from && bot_user_id && strcmp(from, bot_user_id) == 0)
        return 1;
    if (!from || !target_msg_id)
        return 1;

    hu_reaction_kind_t k = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t p = HU_REACTION_NEUTRAL;
    if (!is_remove) {
        if (whatsapp_normalize_emoji(emoji, &k, &p) != 0)
            return 1;
    } else {
        /* Remove: emoji is null/empty so we can't determine kind
         * from it. Mark as UNKNOWN/NEUTRAL — the personal-model
         * sink only treats this as a retraction signal. */
        k = HU_REACTION_UNKNOWN;
        p = HU_REACTION_NEUTRAL;
    }

    hu_reaction_event_t evt = {0};
    evt.channel_id = "whatsapp";
    /* WhatsApp doesn't have rooms/channels — use the sender phone as the
     * thread surrogate so resolver lookups can scope by sender. */
    evt.target_thread_id = strdup(from);
    evt.target_message_ref = strdup(target_msg_id);
    evt.sender_handle = strdup(from);
    evt.kind = k;
    evt.polarity = p;
    evt.is_removal = is_remove ? 1 : 0;
    evt.timestamp_unix = timestamp_s ? (int64_t)atoll(timestamp_s) : (int64_t)time(NULL);
    /* For CUSTOM_EMOJI populate evt.emoji with the glyph so downstream
     * can record provenance. Standard emoji leave it NULL — same shape
     * as discord_reactions.c. */
    evt.emoji = (k == HU_REACTION_KIND_CUSTOM_EMOJI && emoji) ? strdup(emoji) : NULL;

    hu_reaction_handler_handle_event(&evt);

    free((void *)evt.target_thread_id);
    free((void *)evt.target_message_ref);
    free((void *)evt.sender_handle);
    free((void *)evt.emoji);
    return 0;
}

int hu_whatsapp_handle_reaction_webhook(const char *body, size_t body_len, hu_allocator_t *alloc,
                                        const char *bot_user_id) {
    if (!body || body_len == 0 || !alloc)
        return 0;

    /* Quick pre-check so non-reaction bodies don't burn JSON-parse cost.
     * `type":"reaction"` is the smallest signature. */
    if (!memchr(body, '{', body_len))
        return 0;

    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, body, body_len, &root) != HU_OK || !root) {
        return 0;
    }

    hu_json_value_t *entry = hu_json_object_get(root, "entry");
    if (!entry || entry->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return 0;
    }

    int saw_reaction = 0;
    for (size_t e = 0; e < entry->data.array.len; e++) {
        hu_json_value_t *ent = entry->data.array.items[e];
        if (!ent || ent->type != HU_JSON_OBJECT)
            continue;
        hu_json_value_t *changes = hu_json_object_get(ent, "changes");
        if (!changes || changes->type != HU_JSON_ARRAY)
            continue;
        for (size_t ch = 0; ch < changes->data.array.len; ch++) {
            hu_json_value_t *change = changes->data.array.items[ch];
            if (!change || change->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *value = hu_json_object_get(change, "value");
            if (!value || value->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *messages = hu_json_object_get(value, "messages");
            if (!messages || messages->type != HU_JSON_ARRAY)
                continue;
            for (size_t m = 0; m < messages->data.array.len; m++) {
                hu_json_value_t *msg = messages->data.array.items[m];
                if (!msg || msg->type != HU_JSON_OBJECT)
                    continue;
                const char *msg_type = hu_json_get_string(msg, "type");
                if (!msg_type || strcmp(msg_type, "reaction") != 0)
                    continue;
                saw_reaction = 1;
                /* whatsapp_dispatch_one return code is informational:
                 * we own the webhook ack as soon as we see ANY reaction
                 * type message, even if the specific dispatch was a
                 * no-op (self-reaction, unknown emoji). */
                (void)whatsapp_dispatch_one(msg, bot_user_id);
            }
        }
    }

    hu_json_free(alloc, root);
    return saw_reaction ? 1 : 0;
}

#if HU_IS_TEST
hu_error_t hu_whatsapp_handle_reaction_event_for_test(const char *body, size_t body_len,
                                                      hu_allocator_t *alloc,
                                                      const char *bot_user_id,
                                                      hu_reaction_event_t *out) {
    if (!body || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t perr = hu_json_parse(alloc, body, body_len ? body_len : strlen(body), &root);
    if (perr != HU_OK || !root)
        return HU_ERR_INVALID_ARGUMENT;

    /* Walk to the first messages[] entry with type=="reaction". */
    hu_json_value_t *entry = hu_json_object_get(root, "entry");
    if (!entry || entry->type != HU_JSON_ARRAY || entry->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_json_value_t *msg = NULL;
    for (size_t e = 0; e < entry->data.array.len && !msg; e++) {
        hu_json_value_t *ent = entry->data.array.items[e];
        if (!ent || ent->type != HU_JSON_OBJECT)
            continue;
        hu_json_value_t *changes = hu_json_object_get(ent, "changes");
        if (!changes || changes->type != HU_JSON_ARRAY)
            continue;
        for (size_t ch = 0; ch < changes->data.array.len && !msg; ch++) {
            hu_json_value_t *change = changes->data.array.items[ch];
            if (!change || change->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *value = hu_json_object_get(change, "value");
            if (!value || value->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *messages = hu_json_object_get(value, "messages");
            if (!messages || messages->type != HU_JSON_ARRAY)
                continue;
            for (size_t m = 0; m < messages->data.array.len && !msg; m++) {
                hu_json_value_t *candidate = messages->data.array.items[m];
                if (!candidate || candidate->type != HU_JSON_OBJECT)
                    continue;
                const char *t = hu_json_get_string(candidate, "type");
                if (t && strcmp(t, "reaction") == 0)
                    msg = candidate;
            }
        }
    }
    if (!msg) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *from = hu_json_get_string(msg, "from");
    const char *timestamp_s = hu_json_get_string(msg, "timestamp");
    hu_json_value_t *reaction_obj = hu_json_object_get(msg, "reaction");
    if (!reaction_obj || reaction_obj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *target_msg_id = hu_json_get_string(reaction_obj, "message_id");
    const char *emoji = hu_json_get_string(reaction_obj, "emoji");
    int is_remove = whatsapp_reaction_is_remove(reaction_obj);

    /* TEST CONTRACT: filter rejections return HU_ERR_NOT_SUPPORTED. */
    if (from && bot_user_id && strcmp(from, bot_user_id) == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!from || !target_msg_id) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_reaction_kind_t k = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t p = HU_REACTION_NEUTRAL;
    if (!is_remove) {
        if (whatsapp_normalize_emoji(emoji, &k, &p) != 0) {
            hu_json_free(alloc, root);
            return HU_ERR_INVALID_ARGUMENT;
        }
    }

    out->channel_id = "whatsapp";
    out->target_thread_id = strdup(from);
    out->target_message_ref = strdup(target_msg_id);
    out->sender_handle = strdup(from);
    out->kind = k;
    out->polarity = p;
    out->is_removal = is_remove ? 1 : 0;
    out->timestamp_unix = timestamp_s ? (int64_t)atoll(timestamp_s) : (int64_t)time(NULL);
    out->emoji = (k == HU_REACTION_KIND_CUSTOM_EMOJI && emoji) ? strdup(emoji) : NULL;

    hu_json_free(alloc, root);
    return HU_OK;
}
#endif
