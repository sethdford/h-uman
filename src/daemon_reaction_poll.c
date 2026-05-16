#include "human/daemon_reaction_poll.h"

#include "human/channels/reaction_event.h"
#include "human/core/error.h"

#include <stdlib.h>
#include <string.h>

#if HU_IS_TEST
static int *g_imessage_poll_call_counter = NULL;

void hu_daemon_set_poll_call_counter_for_test(int *counter) {
    g_imessage_poll_call_counter = counter;
}

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

hu_error_t hu_daemon_tick_for_test(const hu_config_t *cfg) {
    if (!cfg)
        return HU_ERR_INVALID_ARGUMENT;
    if (!reaction_collection_wants_imessage(cfg))
        return HU_OK;

    const char *db = getenv("HU_CHATDB");
    if (!db || !db[0])
        return HU_OK;

    hu_reaction_event_t events[8];
    size_t n = 0;
    (void)hu_imessage_poll_reactions(db, 0, events, 8, &n);
    if (g_imessage_poll_call_counter)
        (*g_imessage_poll_call_counter)++;
    for (size_t i = 0; i < n; i++) {
        free((void *)events[i].target_thread_id);
        free((void *)events[i].target_message_ref);
        free((void *)events[i].sender_handle);
    }
    return HU_OK;
}
#endif /* HU_IS_TEST */
