/* src/channels/telegram_reactions.c
 *
 * Telegram message_reaction update branch — turns Bot API
 * `message_reaction` updates into normalized hu_reaction_event_t and
 * dispatches them to hu_reaction_handler_handle_event so the personal
 * model picks up Telegram tapbacks the same way it picks up Slack
 * reactions and iMessage tapbacks.
 *
 * This file is split out from src/channels/telegram.c on purpose:
 * human/channels/reaction_event.h reuses the enumerator names
 * HU_REACTION_QUESTION and HU_REACTION_CUSTOM_EMOJI, which already
 * exist in the older hu_reaction_type_t enum declared in
 * human/channel.h. Pulling both headers into the same translation unit
 * is a compile error. telegram.c includes human/channel.h and cannot
 * also include reaction_event.h, so the new branch lives here in a TU
 * that only includes reaction_event.h, never channel.h. Same pattern
 * as imessage_reactions.c (Task 11) and slack_reactions.c (Task 12).
 *
 * Two symbols are exposed (no header — telegram.c uses an extern decl
 * at function scope, the test forward-declares directly):
 *
 *   1. hu_telegram_handle_reaction_update(body, body_len, alloc,
 *                                          bot_user_id)
 *      — invoked from telegram.c's update-dispatch loop BEFORE the
 *      message/channel_post branch. Returns 1 if the update was a
 *      `message_reaction` (caller should advance offset and skip the
 *      message branch), 0 if not. Internal errors silently absorbed.
 *
 *   2. hu_telegram_handle_reaction_event_for_test (HU_IS_TEST only)
 *      — same parse + filter + diff + normalize pipeline but with a
 *      STRICTER return contract that fills a caller-provided
 *      hu_reaction_event_t array (cap-bounded) and reports the count.
 *      Filter rejections come back as HU_ERR_NOT_SUPPORTED so tests
 *      can assert on the filter behavior.
 *
 * Bot API reference: https://core.telegram.org/bots/api#messagereactionupdated
 *
 * Scope notes (documented in code per the brief):
 *   - We do NOT attempt to filter by "was the reacted-to message
 *     authored by the bot." The Bot API surfaces reactions on any
 *     message in chats the bot is a member of, and getUpdates does
 *     not echo the original author. We collect ALL non-self reactions
 *     and let the personal-model MINJA gate handle quality.
 *   - For `type:"custom_emoji"` reactions we populate evt.emoji with
 *     a strdup of the custom_emoji_id. That ID is not a glyph — to
 *     resolve it to a sticker / glyph we would need a follow-up
 *     getCustomEmojiStickers API call. Documented as a known
 *     limitation; the personal-model downstream can either ignore
 *     the ID or treat it as opaque. */

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Telegram emoji glyph → (kind, polarity).
 * Anything unmatched falls through to CUSTOM_EMOJI / NEUTRAL with
 * the glyph preserved on evt.emoji so downstream can still record
 * the reaction provenance. */
static void normalize_telegram_emoji(const char *emoji, hu_reaction_kind_t *kind,
                                     hu_reaction_polarity_t *polarity) {
    /* Heart family — LOVE. The "red heart" can arrive as the bare
     * U+2764 ❤ or with the U+FE0F variation selector ❤️. */
    if (strcmp(emoji, "\xe2\x9d\xa4") == 0 ||             /* ❤ */
        strcmp(emoji, "\xe2\x9d\xa4\xef\xb8\x8f") == 0) { /* ❤️ */
        *kind = HU_REACTION_LOVE;
        *polarity = HU_REACTION_POSITIVE;
        return;
    }
    /* Thumbs up — LIKE */
    if (strcmp(emoji, "\xf0\x9f\x91\x8d") == 0) { /* 👍 */
        *kind = HU_REACTION_LIKE;
        *polarity = HU_REACTION_POSITIVE;
        return;
    }
    /* Thumbs down — DISLIKE */
    if (strcmp(emoji, "\xf0\x9f\x91\x8e") == 0) { /* 👎 */
        *kind = HU_REACTION_DISLIKE;
        *polarity = HU_REACTION_NEGATIVE;
        return;
    }
    /* Laughter family — LAUGH */
    if (strcmp(emoji, "\xf0\x9f\x98\x82") == 0 || /* 😂 */
        strcmp(emoji, "\xf0\x9f\xa4\xa3") == 0 || /* 🤣 */
        strcmp(emoji, "\xf0\x9f\x98\x86") == 0) { /* 😆 */
        *kind = HU_REACTION_LAUGH;
        *polarity = HU_REACTION_POSITIVE;
        return;
    }
    /* Double exclamation — EMPHASIZE */
    if (strcmp(emoji, "\xe2\x80\xbc\xef\xb8\x8f") == 0 || /* ‼️ */
        strcmp(emoji, "\xe2\x80\xbc") == 0) {             /* ‼ */
        *kind = HU_REACTION_EMPHASIZE;
        *polarity = HU_REACTION_POSITIVE;
        return;
    }
    /* Question marks — QUESTION (neutral) */
    if (strcmp(emoji, "\xe2\x9d\x93") == 0 || /* ❓ */
        strcmp(emoji, "\xe2\x9d\x94") == 0) { /* ❔ */
        *kind = HU_REACTION_KIND_QUESTION;
        *polarity = HU_REACTION_NEUTRAL;
        return;
    }
    /* Fall-through: unknown emoji glyph. Use CUSTOM_EMOJI so the
     * personal-model still records the reaction; caller will copy the
     * glyph into evt.emoji for provenance. */
    *kind = HU_REACTION_KIND_CUSTOM_EMOJI;
    *polarity = HU_REACTION_NEUTRAL;
}

/* Extract a (type, key) pair from one reaction array entry. `key` points
 * inside the parsed JSON tree, so do NOT free it. Returns 0 on success,
 * non-zero if the entry is malformed. */
static int reaction_entry_key(const hu_json_value_t *entry, const char **out_type,
                              const char **out_key) {
    if (!entry || entry->type != HU_JSON_OBJECT)
        return 1;
    const char *type = hu_json_get_string(entry, "type");
    if (!type)
        return 1;
    if (strcmp(type, "emoji") == 0) {
        const char *e = hu_json_get_string(entry, "emoji");
        if (!e)
            return 1;
        *out_type = "emoji";
        *out_key = e;
        return 0;
    }
    if (strcmp(type, "custom_emoji") == 0) {
        const char *id = hu_json_get_string(entry, "custom_emoji_id");
        if (!id)
            return 1;
        *out_type = "custom_emoji";
        *out_key = id;
        return 0;
    }
    return 1; /* unknown reaction type */
}

/* Return 1 if (type,key) appears anywhere in `arr`. */
static int reaction_array_contains(const hu_json_value_t *arr, const char *type, const char *key) {
    if (!arr || arr->type != HU_JSON_ARRAY)
        return 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        const char *t = NULL, *k = NULL;
        if (reaction_entry_key(arr->data.array.items[i], &t, &k) != 0)
            continue;
        if (strcmp(t, type) == 0 && strcmp(k, key) == 0)
            return 1;
    }
    return 0;
}

/* Build one hu_reaction_event_t for a single (type,key) entry.
 * Returns 0 on success (out filled with strdup'd strings the caller
 * must free), non-zero on malformed entry. */
static int build_event(const char *r_type, const char *r_key, const char *channel_id_str,
                       const char *message_id_str, const char *sender_handle, int64_t ts_unix,
                       int is_removal, hu_reaction_event_t *out) {
    hu_reaction_kind_t kind;
    hu_reaction_polarity_t polarity;
    const char *emoji_for_event = NULL;
    if (strcmp(r_type, "custom_emoji") == 0) {
        kind = HU_REACTION_KIND_CUSTOM_EMOJI;
        polarity = HU_REACTION_NEUTRAL;
        emoji_for_event = r_key; /* the custom_emoji_id */
    } else {
        normalize_telegram_emoji(r_key, &kind, &polarity);
        if (kind == HU_REACTION_KIND_CUSTOM_EMOJI) {
            /* Unknown glyph: keep it on evt.emoji so downstream can
             * at least record what it was. */
            emoji_for_event = r_key;
        }
    }

    memset(out, 0, sizeof(*out));
    out->channel_id = "telegram";
    out->target_thread_id = channel_id_str ? strdup(channel_id_str) : NULL;
    out->target_message_ref = message_id_str ? strdup(message_id_str) : NULL;
    out->sender_handle = sender_handle ? strdup(sender_handle) : NULL;
    out->kind = kind;
    out->polarity = polarity;
    out->timestamp_unix = ts_unix;
    out->is_removal = is_removal ? 1 : 0;
    out->emoji = emoji_for_event ? strdup(emoji_for_event) : NULL;
    return 0;
}

static void free_event_strings(hu_reaction_event_t *e) {
    free((void *)e->target_thread_id);
    free((void *)e->target_message_ref);
    free((void *)e->sender_handle);
    free((void *)e->emoji);
    memset(e, 0, sizeof(*e));
}

/* Diff/filter/normalize one parsed message_reaction object.
 * `mr` must be the `message_reaction` JSON object (not the whole update).
 * Same return contract as parse_reaction_update below. */
static hu_error_t parse_message_reaction_object(const hu_json_value_t *mr, const char *bot_user_id,
                                                hu_reaction_event_t *out, size_t cap,
                                                size_t *out_n) {
    *out_n = 0;
    if (!mr || mr->type != HU_JSON_OBJECT)
        return HU_ERR_NOT_SUPPORTED;

    /* user.id — filter self-reactions */
    hu_json_value_t *user = hu_json_object_get(mr, "user");
    char user_id_buf[32];
    user_id_buf[0] = '\0';
    const char *username = NULL;
    if (user && user->type == HU_JSON_OBJECT) {
        double id_num = hu_json_get_number(user, "id", 0);
        snprintf(user_id_buf, sizeof(user_id_buf), "%.0f", id_num);
        username = hu_json_get_string(user, "username");
        if (bot_user_id && *bot_user_id && strcmp(user_id_buf, bot_user_id) == 0) {
            return HU_ERR_NOT_SUPPORTED;
        }
    }

    /* chat.id → target_thread_id */
    hu_json_value_t *chat = hu_json_object_get(mr, "chat");
    if (!chat || chat->type != HU_JSON_OBJECT) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    char chat_id_buf[32];
    {
        double chat_id_num = hu_json_get_number(chat, "id", 0);
        snprintf(chat_id_buf, sizeof(chat_id_buf), "%.0f", chat_id_num);
    }

    /* message_id → target_message_ref */
    double mid_num = hu_json_get_number(mr, "message_id", 0);
    char message_id_buf[32];
    snprintf(message_id_buf, sizeof(message_id_buf), "%.0f", mid_num);

    /* date → timestamp_unix. Telegram gives us the actual reaction
     * timestamp; use it rather than time(NULL) so test fixtures and
     * out-of-order delivery still produce a stable ts. */
    double date_num = hu_json_get_number(mr, "date", 0);
    int64_t ts_unix = (int64_t)date_num;

    /* sender handle: prefer @username, fall back to user.id. */
    const char *sender_handle =
        (username && *username) ? username : (user_id_buf[0] ? user_id_buf : NULL);

    /* Diff old_reaction vs new_reaction. Comparison key: (type,
     * emoji-or-custom_emoji_id). */
    hu_json_value_t *old_arr = hu_json_object_get(mr, "old_reaction");
    hu_json_value_t *new_arr = hu_json_object_get(mr, "new_reaction");

    /* ADDs: in new but not in old. */
    if (new_arr && new_arr->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < new_arr->data.array.len && *out_n < cap; i++) {
            const char *t = NULL, *k = NULL;
            if (reaction_entry_key(new_arr->data.array.items[i], &t, &k) != 0)
                continue;
            if (reaction_array_contains(old_arr, t, k))
                continue;
            if (build_event(t, k, chat_id_buf, message_id_buf, sender_handle, ts_unix, 0,
                            &out[*out_n]) == 0) {
                (*out_n)++;
            }
        }
    }

    /* REMOVEs: in old but not in new. */
    if (old_arr && old_arr->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < old_arr->data.array.len && *out_n < cap; i++) {
            const char *t = NULL, *k = NULL;
            if (reaction_entry_key(old_arr->data.array.items[i], &t, &k) != 0)
                continue;
            if (reaction_array_contains(new_arr, t, k))
                continue;
            if (build_event(t, k, chat_id_buf, message_id_buf, sender_handle, ts_unix, 1,
                            &out[*out_n]) == 0) {
                (*out_n)++;
            }
        }
    }

    return HU_OK;
}

/* Wrapper that parses a whole update body and routes the
 * `message_reaction` subobject. Returns:
 *   HU_OK                  — parsed cleanly (out_n may be 0).
 *   HU_ERR_NOT_SUPPORTED   — no message_reaction field, or self-reaction.
 *   HU_ERR_INVALID_ARGUMENT — malformed JSON / missing required fields. */
static hu_error_t parse_reaction_update(const char *body, size_t body_len, hu_allocator_t *alloc,
                                        const char *bot_user_id, hu_reaction_event_t *out,
                                        size_t cap, size_t *out_n) {
    *out_n = 0;
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, body, body_len, &root) != HU_OK || !root) {
        if (root)
            hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_json_value_t *mr = hu_json_object_get(root, "message_reaction");
    if (!mr) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }
    hu_error_t err = parse_message_reaction_object(mr, bot_user_id, out, cap, out_n);
    hu_json_free(alloc, root);
    return err;
}

/* Dispatch a parsed message_reaction object — telegram.c's update loop
 * already has the update parsed, so it can call this directly without
 * re-parsing the body. Returns 1 if `up` was a message_reaction update
 * (caller should treat as handled and skip its message branch), 0
 * otherwise. */
int hu_telegram_dispatch_reaction_from_update(const hu_json_value_t *up, const char *bot_user_id) {
    if (!up || up->type != HU_JSON_OBJECT)
        return 0;
    hu_json_value_t *mr = hu_json_object_get(up, "message_reaction");
    if (!mr)
        return 0;
    hu_reaction_event_t events[16];
    size_t n = 0;
    hu_error_t err = parse_message_reaction_object(mr, bot_user_id, events, 16, &n);
    if (err != HU_OK) {
        /* Self-reaction filter, malformed reaction object — still own
         * the update so caller skips its message branch. */
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        hu_reaction_handler_handle_event(&events[i]);
        free_event_strings(&events[i]);
    }
    return 1;
}

int hu_telegram_handle_reaction_update(const char *body, size_t body_len, hu_allocator_t *alloc,
                                       const char *bot_user_id) {
    if (!body || body_len == 0 || !alloc)
        return 0;

    /* Quick pre-check so non-reaction updates don't burn JSON-parse cost.
     * The parse_reaction_update call below will re-confirm and return
     * HU_ERR_NOT_SUPPORTED if the key is absent for any reason. */
    if (!memchr(body, '{', body_len) || !memmem(body, body_len, "message_reaction", 16)) {
        return 0;
    }

    hu_reaction_event_t events[16];
    size_t n = 0;
    hu_error_t err = parse_reaction_update(body, body_len, alloc, bot_user_id, events, 16, &n);
    if (err == HU_ERR_NOT_SUPPORTED) {
        /* Either not a message_reaction update, or filtered (self). For
         * "not a message_reaction" return 0 so the caller falls through
         * to its normal message branch; for "self-reaction" return 1
         * because the caller already advanced the update_id offset and
         * we own the dispatch for this update. The two are indistinguish-
         * able at this layer; safe default is to fall through (return 0)
         * — telegram.c's message-branch will then no-op because there's
         * no "message" field on a message_reaction update. */
        return 0;
    }
    if (err != HU_OK) {
        /* Malformed: silently absorb so a single bad update doesn't kill
         * the polling loop, but tell the caller we owned this update so
         * it doesn't double-process. */
        return 1;
    }

    /* Dispatch each event to the reaction_handler. Return code is
     * informational only — the handler resolves (channel, thread,
     * msg_ref) → assistant message and writes a dpo_pairs row when the
     * lookup hits and polarity is non-neutral. */
    for (size_t i = 0; i < n; i++) {
        hu_reaction_handler_handle_event(&events[i]);
        free_event_strings(&events[i]);
    }
    return 1;
}

#if HU_IS_TEST
hu_error_t hu_telegram_handle_reaction_event_for_test(const char *body, size_t body_len,
                                                      hu_allocator_t *alloc,
                                                      const char *bot_user_id,
                                                      hu_reaction_event_t *out_events, size_t cap,
                                                      size_t *out_n) {
    if (!body || !alloc || !out_events || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    return parse_reaction_update(body, body_len, alloc, bot_user_id, out_events, cap, out_n);
}
#endif
