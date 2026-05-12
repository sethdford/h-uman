/* src/channels/slack_reactions.c
 *
 * Phase 2 Task 12 (RL SOTA): Slack reaction_added / reaction_removed
 * webhook branch + test-only helper.
 *
 * This file is split out from src/channels/slack.c on purpose:
 * human/channels/reaction_event.h (introduced by Task 10) reuses the
 * enumerator names HU_REACTION_QUESTION and HU_REACTION_CUSTOM_EMOJI,
 * which already exist in the older hu_reaction_type_t enum declared in
 * human/channel.h. Pulling both headers into the same translation unit
 * is a compile error. slack.c includes human/channel.h (line 5) and
 * cannot also include reaction_event.h, so the new branch lives here in
 * a TU that only includes reaction_event.h, never channel.h. Same
 * pattern as Task 11's src/channels/imessage_reactions.c.
 *
 * Two symbols are exposed (no header — slack.c uses an extern decl at
 * function scope, the test forward-declares directly):
 *
 *   1. hu_slack_handle_reaction_webhook(body, body_len, alloc, bot_user_id)
 *      — invoked from slack.c BEFORE the existing event_type=="message"
 *      filter (which would otherwise swallow reaction events). Returns 1
 *      if the body was a reaction event the caller should ack and stop;
 *      0 if the body was not a reaction event (caller continues normal
 *      processing). NEVER returns a non-zero error: any internal failure
 *      (parse error, unknown reactji, self-reaction, non-message item)
 *      returns 1 with silent absorption — Slack retries on non-200
 *      within 3s, so once we recognise the event_type as a reaction we
 *      OWN the response.
 *      NOTE: This re-parses the body. Wasteful vs. passing
 *      hu_json_value_t*, but avoids leaking reaction_event.h into
 *      slack.c, which would collide with channel.h's hu_reaction_type_t
 *      enumerators.
 *
 *   2. hu_slack_handle_reaction_event_for_test (HU_IS_TEST only) —
 *      the same parse/filter/normalize pipeline but with a STRICTER
 *      return contract: filter rejections come back as
 *      HU_ERR_NOT_SUPPORTED so unit tests can assert on the filter
 *      behavior. The inline webhook branch translates the same
 *      rejections to a silent ack. Both contracts are intentional —
 *      see test file for the asserts that depend on the helper's
 *      contract. */

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int hu_slack_handle_reaction_webhook(const char *body, size_t body_len,
                                     hu_allocator_t *alloc,
                                     const char *bot_user_id) {
    if (!body || body_len == 0 || !alloc) return 0;

    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, body, body_len, &root) != HU_OK || !root) {
        return 0;
    }

    hu_json_value_t *event = hu_json_object_get(root, "event");
    if (!event) {
        hu_json_free(alloc, root);
        return 0;
    }

    const char *event_type = hu_json_get_string(event, "type");
    int is_added = event_type && strcmp(event_type, "reaction_added") == 0;
    int is_removed = event_type && strcmp(event_type, "reaction_removed") == 0;
    if (!is_added && !is_removed) {
        /* Not a reaction event — fall through to slack.c's existing
         * dispatch (message handling, url_verification, etc.). */
        hu_json_free(alloc, root);
        return 0;
    }

    /* From this point on, the event_type is reaction_added or
     * reaction_removed. We OWN the webhook response: return 1 on every
     * exit so slack.c acks with HU_OK and Slack does not retry. */

    hu_reaction_event_t evt = {0};

    const char *reaction_name = hu_json_get_string(event, "reaction");
    const char *user = hu_json_get_string(event, "user");
    hu_json_value_t *item = hu_json_object_get(event, "item");
    const char *item_type = item ? hu_json_get_string(item, "type") : NULL;
    const char *item_channel = item ? hu_json_get_string(item, "channel") : NULL;
    const char *item_ts = item ? hu_json_get_string(item, "ts") : NULL;

    /* Filter: skip self-reactions, non-message items, missing reactji.
     * Each rejection silently acks the webhook (return 1) — see the
     * file-level comment for why. The test helper below has the
     * stricter HU_ERR_NOT_SUPPORTED contract for these same cases. */
    if (user && bot_user_id && strcmp(user, bot_user_id) == 0) goto done;
    if (!item_type || strcmp(item_type, "message") != 0)        goto done;
    if (!reaction_name)                                          goto done;

    hu_reaction_kind_t k;
    hu_reaction_polarity_t p;
    if (hu_reaction_normalize_slack(reaction_name, &k, &p) != HU_OK) goto done;

    evt.channel_id = "slack";
    evt.target_thread_id = item_channel ? strdup(item_channel) : NULL;
    evt.target_message_ref = item_ts ? strdup(item_ts) : NULL;
    evt.sender_handle = user ? strdup(user) : NULL;
    evt.kind = k;
    evt.polarity = p;
    evt.is_removal = is_removed ? 1 : 0;
    evt.timestamp_unix = time(NULL);

    /* Phase 2 Task 13: dispatch the event to the reaction_handler. The
     * handler resolves (channel, thread, msg_ref) → assistant message
     * and writes a dpo_pairs row when the lookup hits and polarity is
     * non-neutral. Return code is informational only — Slack retries on
     * non-200 within 3s, so we own the webhook ack regardless of whether
     * the handler resolved the lookup, recorded the pair, or returned
     * HU_ERR_NOT_FOUND because the message predates the in-memory
     * lookup window (R4 in the risk register). */
    hu_reaction_handler_handle_event(&evt);  /* fire-and-forget; return code is informational only */

done:
    /* NULL-safe: strdup may not have been called if a filter goto'd here. */
    free((void *)evt.target_thread_id);
    free((void *)evt.target_message_ref);
    free((void *)evt.sender_handle);
    hu_json_free(alloc, root);
    return 1;
}

#if HU_IS_TEST
hu_error_t hu_slack_handle_reaction_event_for_test(const char *payload,
                                                    hu_allocator_t *alloc,
                                                    const char *bot_user_id,
                                                    hu_reaction_event_t *out) {
    if (!payload || !alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t perr = hu_json_parse(alloc, payload, strlen(payload), &root);
    if (perr != HU_OK || !root) return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *event_obj = hu_json_object_get(root, "event");
    if (!event_obj) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *event_type = hu_json_get_string(event_obj, "type");
    int is_removal = event_type && strcmp(event_type, "reaction_removed") == 0;
    if (!event_type || (strcmp(event_type, "reaction_added") != 0 && !is_removal)) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *reaction_name = hu_json_get_string(event_obj, "reaction");
    const char *user = hu_json_get_string(event_obj, "user");
    hu_json_value_t *item = hu_json_object_get(event_obj, "item");
    const char *item_type = item ? hu_json_get_string(item, "type") : NULL;
    const char *item_channel = item ? hu_json_get_string(item, "channel") : NULL;
    const char *item_ts = item ? hu_json_get_string(item, "ts") : NULL;

    /* TEST CONTRACT (different from inline webhook branch above):
     * filter rejections return HU_ERR_NOT_SUPPORTED so the two filter
     * tests can assert on the filter behavior. The inline webhook
     * branch translates these to HU_OK acks — that is intentional. */
    if (user && bot_user_id && strcmp(user, bot_user_id) == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!item_type || strcmp(item_type, "message") != 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!reaction_name) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_reaction_kind_t k;
    hu_reaction_polarity_t p;
    if (hu_reaction_normalize_slack(reaction_name, &k, &p) != HU_OK) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    out->channel_id = "slack";
    out->target_thread_id = item_channel ? strdup(item_channel) : NULL;
    out->target_message_ref = item_ts ? strdup(item_ts) : NULL;
    out->sender_handle = user ? strdup(user) : NULL;
    out->kind = k;
    out->polarity = p;
    out->is_removal = is_removal ? 1 : 0;
    out->timestamp_unix = time(NULL);

    hu_json_free(alloc, root);
    return HU_OK;
}
#endif
