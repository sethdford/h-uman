/* src/daemon_reaction_poll.c
 *
 * CF-3 closure: the iMessage reaction poll tick was previously gated
 * under #if HU_IS_TEST and discarded events with (void). Now exposed
 * as `hu_daemon_reaction_poll_tick` (production-callable) which
 * additionally feeds each event into `hu_reaction_handler_handle_event`
 * so it lands in the daemon-owned hu_dpo_collector_t.
 */

#include "human/daemon_reaction_poll.h"

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

void hu_daemon_reaction_wire_collector(struct hu_dpo_collector *collector) {
    hu_reaction_handler_set_collector((hu_dpo_collector_t *)collector);
}

#if HU_IS_TEST
static int *g_imessage_poll_call_counter = NULL;

void hu_daemon_set_poll_call_counter_for_test(int *counter) {
    g_imessage_poll_call_counter = counter;
}
#endif

extern hu_error_t hu_imessage_poll_reactions(const char *db_path, int64_t since_unix,
                                             hu_reaction_event_t *out, size_t cap, size_t *out_n);

static bool reaction_collection_wants_imessage(const hu_config_t *cfg) {
    if (!cfg || !cfg->reaction_collection.enabled)
        return false;
    if (cfg->reaction_collection.channel_count == 0)
        return true;
    for (size_t i = 0; i < cfg->reaction_collection.channel_count; i++) {
        if (strcmp(cfg->reaction_collection.channels[i], "imessage") == 0)
            return true;
    }
    return false;
}

static void free_event_strings(hu_reaction_event_t *ev) {
    free((void *)ev->target_thread_id);
    free((void *)ev->target_message_ref);
    free((void *)ev->sender_handle);
    ev->target_thread_id = NULL;
    ev->target_message_ref = NULL;
    ev->sender_handle = NULL;
}

hu_error_t hu_daemon_reaction_poll_tick(const hu_config_t *cfg,
                                        int64_t since_unix,
                                        size_t *out_ingested) {
    if (out_ingested) *out_ingested = 0;
    if (!cfg)
        return HU_OK;
    if (!reaction_collection_wants_imessage(cfg))
        return HU_OK;

    const char *db = getenv("HU_CHATDB");
    if (!db || !db[0])
        return HU_OK;

    hu_reaction_event_t events[16];
    size_t n = 0;
    hu_error_t pe = hu_imessage_poll_reactions(db, since_unix, events, 16, &n);

#if HU_IS_TEST
    if (g_imessage_poll_call_counter)
        (*g_imessage_poll_call_counter)++;
#endif

    /* HU_ERR_NOT_SUPPORTED is expected on non-macOS / non-SQLite
     * builds and under HU_IS_TEST -- treat it as "nothing to poll"
     * rather than a hard error, so the daemon's tick stays quiet. */
    if (pe != HU_OK && pe != HU_ERR_NOT_SUPPORTED) {
        for (size_t i = 0; i < n; i++) free_event_strings(&events[i]);
        return pe;
    }

    size_t ingested = 0;
    for (size_t i = 0; i < n; i++) {
        if (events[i].channel_id == NULL)
            events[i].channel_id = "imessage";
        hu_error_t he = hu_reaction_handler_handle_event(&events[i]);
        if (he == HU_OK) ingested++;
        free_event_strings(&events[i]);
    }
    if (out_ingested) *out_ingested = ingested;
    return HU_OK;
}

#if HU_IS_TEST
hu_error_t hu_daemon_tick_for_test(const hu_config_t *cfg) {
    if (!cfg)
        return HU_ERR_INVALID_ARGUMENT;
    /* Preserve the original test entry's strict-arg validation
     * (returns INVALID_ARGUMENT on NULL cfg) while delegating the
     * poll loop to the production path so both share the same code. */
    return hu_daemon_reaction_poll_tick(cfg, 0, NULL);
}
#endif
