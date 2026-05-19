/* src/channels/discord_reactions.c
 *
 * RL SOTA — Discord MESSAGE_REACTION_ADD / MESSAGE_REACTION_REMOVE
 * gateway-event branch + test-only helper.
 *
 * This file is split out from src/channels/discord.c on purpose: the
 * channel.h hu_reaction_type_t enum (used by discord.c's outbound
 * reactions.add path) and the human/channels/reaction_event.h
 * hu_reaction_kind_t enum reuse enumerator names HU_REACTION_QUESTION
 * and HU_REACTION_CUSTOM_EMOJI. Pulling both headers into the same
 * translation unit is a compile error, so this branch lives in a TU
 * that only includes reaction_event.h, never channel.h. Same pattern
 * as slack_reactions.c (Task 12) and imessage_reactions.c (Task 11).
 *
 * Two symbols are exposed (no header — discord.c uses an extern decl
 * at function scope, the test forward-declares directly):
 *
 *   1. hu_discord_handle_reaction_gateway(body, body_len, alloc, bot_user_id)
 *      — invoked from discord.c's gateway-style dispatcher BEFORE the
 *      existing MESSAGE_CREATE filter. Returns 1 if the body was a
 *      reaction event the caller should stop processing; 0 if not
 *      (caller continues). NEVER returns a negative value: internal
 *      failures (malformed JSON, missing fields, filter rejection)
 *      silently absorb as 1 once the dispatcher's `t` field has been
 *      recognized as a reaction event — Discord retries on non-200
 *      webhook responses just like Slack does.
 *
 *   2. hu_discord_handle_reaction_event_for_test (HU_IS_TEST only) —
 *      same parse/filter/normalize pipeline but with a stricter return
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

/* Map a Discord standard-emoji `name` (the emoji.name JSON field — a raw
 * unicode glyph for standard emoji; a colon-name like "blobwave" for
 * custom emoji) to a hu_reaction_kind_t / polarity pair.
 *
 * For custom emoji (caller passes is_custom=1, i.e. emoji.id != null),
 * we never consult the name table and always return CUSTOM_EMOJI. */
static hu_error_t discord_normalize_emoji(const char *name, int is_custom,
                                          hu_reaction_kind_t *out_kind,
                                          hu_reaction_polarity_t *out_polarity) {
    if (!out_kind || !out_polarity)
        return HU_ERR_INVALID_ARGUMENT;
    *out_kind = HU_REACTION_UNKNOWN;
    *out_polarity = HU_REACTION_NEUTRAL;
    if (is_custom) {
        *out_kind = HU_REACTION_KIND_CUSTOM_EMOJI;
        *out_polarity = HU_REACTION_POSITIVE;
        return HU_OK;
    }
    if (!name || !*name)
        return HU_ERR_INVALID_ARGUMENT;

    /* Substring matches against the raw UTF-8 glyph — picks up skin-tone
     * variants (👍🏻..👍🏿) since the base codepoint precedes the modifier.
     * This is the same coarse approach Slack uses; precision isn't load-
     * bearing because the personal-model only cares about polarity. */
    if (strstr(name, "\xe2\x9d\xa4") /* ❤ */ || strstr(name, "\xf0\x9f\x92\x95") /* 💕 */ ||
        strstr(name, "\xf0\x9f\x92\x96") /* 💖 */ || strstr(name, "\xf0\x9f\x92\x98") /* 💘 */) {
        *out_kind = HU_REACTION_LOVE;
        *out_polarity = HU_REACTION_POSITIVE;
        return HU_OK;
    }
    if (strstr(name, "\xf0\x9f\x91\x8d") /* 👍 */) {
        *out_kind = HU_REACTION_LIKE;
        *out_polarity = HU_REACTION_POSITIVE;
        return HU_OK;
    }
    if (strstr(name, "\xf0\x9f\x91\x8e") /* 👎 */) {
        *out_kind = HU_REACTION_DISLIKE;
        *out_polarity = HU_REACTION_NEGATIVE;
        return HU_OK;
    }
    if (strstr(name, "\xf0\x9f\x98\x82") /* 😂 */ || strstr(name, "\xf0\x9f\xa4\xa3") /* 🤣 */ ||
        strstr(name, "\xf0\x9f\x98\x86") /* 😆 */) {
        *out_kind = HU_REACTION_LAUGH;
        *out_polarity = HU_REACTION_POSITIVE;
        return HU_OK;
    }
    if (strstr(name, "\xe2\x80\xbc") /* ‼ */ || strstr(name, "\xe2\x9d\x97") /* ❗ */ ||
        strstr(name, "\xe2\x9d\x95") /* ❕ */) {
        *out_kind = HU_REACTION_EMPHASIZE;
        *out_polarity = HU_REACTION_POSITIVE;
        return HU_OK;
    }
    if (strstr(name, "\xe2\x9d\x93") /* ❓ */ || strstr(name, "\xe2\x9d\x94") /* ❔ */ ||
        strstr(name, "\xf0\x9f\xa4\x94") /* 🤔 */) {
        *out_kind = HU_REACTION_KIND_QUESTION;
        *out_polarity = HU_REACTION_NEUTRAL;
        return HU_OK;
    }
    return HU_ERR_INVALID_ARGUMENT;
}

/* Decide if the emoji object encodes a custom (server) emoji.
 * Discord sets emoji.id to a snowflake string for custom emoji, and
 * to JSON null for standard unicode emoji. Our JSON parser surfaces
 * null as either absent or as HU_JSON_NULL — both must register as
 * "not custom". A string-typed id is the custom signal. */
static int discord_emoji_is_custom(const hu_json_value_t *emoji_obj) {
    if (!emoji_obj || emoji_obj->type != HU_JSON_OBJECT)
        return 0;
    const hu_json_value_t *id = hu_json_object_get(emoji_obj, "id");
    if (!id)
        return 0;
    if (id->type == HU_JSON_STRING && id->data.string.ptr && id->data.string.len > 0)
        return 1;
    return 0;
}

int hu_discord_handle_reaction_gateway(const char *body, size_t body_len, hu_allocator_t *alloc,
                                       const char *bot_user_id) {
    if (!body || body_len == 0 || !alloc)
        return 0;

    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, body, body_len, &root) != HU_OK || !root) {
        return 0;
    }

    const char *t = hu_json_get_string(root, "t");
    int is_add = t && strcmp(t, "MESSAGE_REACTION_ADD") == 0;
    int is_remove = t && strcmp(t, "MESSAGE_REACTION_REMOVE") == 0;
    if (!is_add && !is_remove) {
        /* Not a reaction event — let discord.c's normal dispatch handle it. */
        hu_json_free(alloc, root);
        return 0;
    }

    /* From here on, `t` is a reaction event we own. Return 1 on every
     * exit so the caller does not double-dispatch to MESSAGE_CREATE
     * handling, mirroring slack_reactions.c's contract. */

    hu_reaction_event_t evt = {0};
    int is_custom = 0;
    const char *emoji_name = NULL;

    hu_json_value_t *d = hu_json_object_get(root, "d");
    if (!d || d->type != HU_JSON_OBJECT)
        goto done;

    const char *user_id = hu_json_get_string(d, "user_id");
    const char *message_id = hu_json_get_string(d, "message_id");
    const char *channel_id = hu_json_get_string(d, "channel_id");
    const char *message_author_id = hu_json_get_string(d, "message_author_id");
    hu_json_value_t *emoji_obj = hu_json_object_get(d, "emoji");
    emoji_name = emoji_obj ? hu_json_get_string(emoji_obj, "name") : NULL;
    is_custom = discord_emoji_is_custom(emoji_obj);

    /* Filter: skip self-reactions; only collect reactions on the bot's
     * own outbound messages (mirrors slack_reactions.c semantics). When
     * message_author_id is absent, Discord didn't tell us who authored
     * the target — drop conservatively, since we'd otherwise feed noise
     * into the personal model. */
    if (user_id && bot_user_id && strcmp(user_id, bot_user_id) == 0)
        goto done;
    if (!message_author_id || !bot_user_id || strcmp(message_author_id, bot_user_id) != 0)
        goto done;
    if (!message_id || !channel_id)
        goto done;

    hu_reaction_kind_t k;
    hu_reaction_polarity_t p;
    if (discord_normalize_emoji(emoji_name, is_custom, &k, &p) != HU_OK)
        goto done;

    evt.channel_id = "discord";
    evt.target_thread_id = strdup(channel_id);
    evt.target_message_ref = strdup(message_id);
    evt.sender_handle = user_id ? strdup(user_id) : NULL;
    evt.kind = k;
    evt.polarity = p;
    evt.is_removal = is_remove ? 1 : 0;
    /* Discord MESSAGE_REACTION_ADD events do not carry a top-level
     * server timestamp the way Slack events do — use wall-clock at
     * receive time, same as the imessage poll branch. */
    evt.timestamp_unix = (int64_t)time(NULL);
    evt.emoji = (is_custom && emoji_name) ? strdup(emoji_name) : NULL;

    /* Fire-and-forget; return code is informational only. The handler
     * resolves (channel, thread, msg_ref) -> assistant message and
     * writes a dpo_pairs row when the lookup hits and polarity is
     * non-neutral. */
    hu_reaction_handler_handle_event(&evt);

done:
    free((void *)evt.target_thread_id);
    free((void *)evt.target_message_ref);
    free((void *)evt.sender_handle);
    free((void *)evt.emoji);
    hu_json_free(alloc, root);
    return 1;
}

#if HU_IS_TEST
hu_error_t hu_discord_handle_reaction_event_for_test(const char *payload, size_t payload_len,
                                                     hu_allocator_t *alloc, const char *bot_user_id,
                                                     hu_reaction_event_t *out) {
    if (!payload || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t perr =
        hu_json_parse(alloc, payload, payload_len ? payload_len : strlen(payload), &root);
    if (perr != HU_OK || !root)
        return HU_ERR_INVALID_ARGUMENT;

    const char *t = hu_json_get_string(root, "t");
    int is_add = t && strcmp(t, "MESSAGE_REACTION_ADD") == 0;
    int is_remove = t && strcmp(t, "MESSAGE_REACTION_REMOVE") == 0;
    if (!is_add && !is_remove) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_json_value_t *d = hu_json_object_get(root, "d");
    if (!d || d->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *user_id = hu_json_get_string(d, "user_id");
    const char *message_id = hu_json_get_string(d, "message_id");
    const char *channel_id = hu_json_get_string(d, "channel_id");
    const char *message_author_id = hu_json_get_string(d, "message_author_id");
    hu_json_value_t *emoji_obj = hu_json_object_get(d, "emoji");
    const char *emoji_name = emoji_obj ? hu_json_get_string(emoji_obj, "name") : NULL;
    int is_custom = discord_emoji_is_custom(emoji_obj);

    /* TEST CONTRACT: filter rejections return HU_ERR_NOT_SUPPORTED so
     * tests can pin filter behavior. */
    if (user_id && bot_user_id && strcmp(user_id, bot_user_id) == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!message_author_id || !bot_user_id || strcmp(message_author_id, bot_user_id) != 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!message_id || !channel_id) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_reaction_kind_t k;
    hu_reaction_polarity_t p;
    if (discord_normalize_emoji(emoji_name, is_custom, &k, &p) != HU_OK) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    out->channel_id = "discord";
    out->target_thread_id = strdup(channel_id);
    out->target_message_ref = strdup(message_id);
    out->sender_handle = user_id ? strdup(user_id) : NULL;
    out->kind = k;
    out->polarity = p;
    out->is_removal = is_remove ? 1 : 0;
    out->timestamp_unix = (int64_t)time(NULL);
    out->emoji = (is_custom && emoji_name) ? strdup(emoji_name) : NULL;

    hu_json_free(alloc, root);
    return HU_OK;
}
#endif
