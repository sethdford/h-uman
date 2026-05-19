/* src/agent/reaction_handler.c
 *
 * Phase 2 Task 13 (RL SOTA): hu_reaction_event_t → hu_preference_pair_t
 * row in the daemon-owned hu_dpo_collector_t. See the header for the
 * full wiring diagram and the lookup-cap caveat. */
#include "human/agent/reaction_handler.h"
#include "human/channels/imessage_ingest.h"
#include "human/memory/personal_model.h"
#include "human/ml/dpo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CAVEAT: in-memory lookup, 256-entry cap, NOT persisted. This is the test
 * seam + interim production path until the daemon-side assistant-message
 * resolver lands in Phase 5. Reactions on messages older than the most
 * recent 256 sends silently drop (R4 in the risk register). */
#define LOOKUP_CAP 256

typedef struct {
    char channel[32];
    char thread[128];
    char msg_ref[128];
    char prompt[2048];
    char response[4096];
} lookup_entry_t;

static lookup_entry_t s_lookup[LOOKUP_CAP];
static size_t s_lookup_n = 0;

/* Daemon-owned collector handle. NULL until set_collector is called. */
static hu_dpo_collector_t *s_collector = NULL;

/* Phase 1c of docs/plans/2026-05-18-imessage-sota.md: optional personal-model
 * sink. When non-NULL, iMessage reactions on registered assistant messages
 * are also ingested into the personal model (separate from the DPO collector
 * which exists for training-data collection). Mirrors the set_collector
 * pattern: daemon sets at init via hu_daemon_reaction_wire_personal_model. */
static hu_personal_model_t *s_personal_model = NULL;

/* Per-turn signal flag (NOT thread-safe; daemon is single-threaded event loop —
 * see header comment on hu_reaction_handler_clear_turn for the full safety
 * argument. If the daemon ever gains concurrent turn dispatch, move this onto
 * hu_agent_t as a per-agent field). */
static int s_called_this_turn = 0;

void hu_reaction_handler_set_collector(hu_dpo_collector_t *c) {
    s_collector = c;
}

void hu_reaction_handler_set_personal_model(hu_personal_model_t *m) {
    s_personal_model = m;
}
void hu_reaction_handler_clear_turn(void) {
    s_called_this_turn = 0;
}
int hu_reaction_handler_was_called_this_turn(void) {
    return s_called_this_turn;
}

static const lookup_entry_t *find_lookup(const hu_reaction_event_t *e) {
    for (size_t i = 0; i < s_lookup_n; i++) {
        if (strcmp(s_lookup[i].channel, e->channel_id) == 0 &&
            strcmp(s_lookup[i].thread, e->target_thread_id ? e->target_thread_id : "") == 0 &&
            strcmp(s_lookup[i].msg_ref, e->target_message_ref ? e->target_message_ref : "") == 0)
            return &s_lookup[i];
    }
    return NULL;
}

hu_error_t hu_reaction_handler_handle_event(const hu_reaction_event_t *e) {
    if (!e || !e->channel_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (e->is_removal)
        return HU_OK; /* drop removals; we only record adds */
    const lookup_entry_t *L = find_lookup(e);
    if (!L)
        return HU_ERR_NOT_FOUND;

    /* Phase 1c of docs/plans/2026-05-18-imessage-sota.md: iMessage reactions
     * on registered assistant messages also feed the personal model. This
     * happens BEFORE the DPO record so that a missing DPO collector (e.g.
     * RL build flag off) does not block personal-model learning. */
    if (s_personal_model) {
        /* Phase 2 of docs/plans/2026-05-18-imessage-sota.md: channel-
         * agnostic ingest. The synthesis renders "<actor> reacted with
         * <glyph> to <target>" regardless of channel; provenance is
         * built from event->channel_id by event_provenance. Slack +
         * any future reaction-bearing channel reaches this point via
         * hu_reaction_handler_handle_event without further wiring.
         *
         * L->response is the text the reactor reacted to (our assistant
         * message). is_from_me_target=true because the lookup only holds
         * our own outbound messages by construction. e->emoji carries
         * the iOS 17+ custom-emoji glyph when present (NULL otherwise —
         * synth_reaction falls back to "a sticker"). */
        (void)hu_imessage_ingest_reaction(s_personal_model, e,
                                          /*custom_emoji=*/e->emoji, L->response,
                                          /*is_from_me_target=*/true,
                                          /*in_group_chat=*/false);
    }

    if (!s_collector)
        return HU_ERR_NOT_SUPPORTED; /* daemon hasn't wired it yet */

    /* Build source string. hu_preference_pair_t.source is a char[64], so we
     * write into the struct directly (NOT a const char* assignment — that
     * would be a C11 type error since the field is an array, not a pointer). */
    hu_preference_pair_t pair = {0};

    /* Pick source string per channel */
    const char *src = "unknown";
    if (strcmp(e->channel_id, "imessage") == 0)
        src = "imessage_tapback";
    else if (strcmp(e->channel_id, "slack") == 0)
        src = "slack_reactji";
    else
        src = e->channel_id;

    /* Copy strings into fixed-size buffers (NOT pointer assignment — fields
     * are char[2048] / char[4096] / char[64] per include/human/ml/dpo.h:15-26). */
    strncpy(pair.prompt, L->prompt, sizeof(pair.prompt) - 1);
    pair.prompt_len = strlen(pair.prompt);

    if (e->polarity > 0) {
        /* Positive reaction → record this response as `chosen` */
        strncpy(pair.chosen, L->response, sizeof(pair.chosen) - 1);
        pair.chosen_len = strlen(pair.chosen);
        /* `rejected` left as zeroed-out empty string */
    } else if (e->polarity < 0) {
        /* Negative reaction → record this response as `rejected` */
        strncpy(pair.rejected, L->response, sizeof(pair.rejected) - 1);
        pair.rejected_len = strlen(pair.rejected);
    } else {
        return HU_OK; /* neutral reactions don't yield training signal */
    }

    pair.margin = (double)e->polarity;
    pair.timestamp = e->timestamp_unix;
    strncpy(pair.source, src, sizeof(pair.source) - 1);
    pair.source_len = strlen(pair.source);

    /* Set the per-turn flag BEFORE hu_dpo_record_pair so that even if the
     * SQLite insert fails (disk full, schema drift, etc.) the agent_turn
     * code path knows a reaction was observed this turn — the substring
     * heuristic should still defer. The return code is the caller's
     * diagnostic; the flag is the side-effect signal. */
    s_called_this_turn = 1;
    return hu_dpo_record_pair(s_collector, &pair);
}

static void register_assistant_message(const char *channel, const char *thread, const char *msg_ref,
                                       const char *prompt, const char *response) {
    if (!channel || !thread || !msg_ref || !prompt || !response)
        return;
    if (s_lookup_n >= LOOKUP_CAP)
        return;
    snprintf(s_lookup[s_lookup_n].channel, sizeof(s_lookup[0].channel), "%s", channel);
    snprintf(s_lookup[s_lookup_n].thread, sizeof(s_lookup[0].thread), "%s", thread);
    snprintf(s_lookup[s_lookup_n].msg_ref, sizeof(s_lookup[0].msg_ref), "%s", msg_ref);
    snprintf(s_lookup[s_lookup_n].prompt, sizeof(s_lookup[0].prompt), "%s", prompt);
    snprintf(s_lookup[s_lookup_n].response, sizeof(s_lookup[0].response), "%s", response);
    s_lookup_n++;
}

void hu_reaction_handler_register_assistant_message_for_production(const char *channel,
                                                                   const char *thread,
                                                                   const char *msg_ref,
                                                                   const char *prompt,
                                                                   const char *response) {
    register_assistant_message(channel, thread, msg_ref, prompt, response);
}

#if HU_IS_TEST
void hu_reaction_handler_register_assistant_message_for_test(const char *channel,
                                                             const char *thread,
                                                             const char *msg_ref,
                                                             const char *prompt,
                                                             const char *response) {
    register_assistant_message(channel, thread, msg_ref, prompt, response);
}
void hu_reaction_handler_reset_for_test(void) {
    s_lookup_n = 0;
    s_called_this_turn = 0;
    s_collector = NULL;
    s_personal_model = NULL;
}
#endif
