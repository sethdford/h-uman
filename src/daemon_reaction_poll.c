#include "human/daemon_reaction_poll.h"

#include "human/agent/reaction_handler.h"
#include "human/channels/imessage_reactions.h"
#include "human/core/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HU_IS_TEST
static int *g_imessage_poll_call_counter = NULL;
static int g_poll_count_for_test = 0;

void hu_daemon_set_poll_call_counter_for_test(int *counter) {
    g_imessage_poll_call_counter = counter;
}

int hu_daemon_reaction_poll_get_count_for_test(void) { return g_poll_count_for_test; }

void hu_daemon_reaction_poll_reset_count_for_test(void) { g_poll_count_for_test = 0; }
#endif

static bool reaction_collection_wants_imessage(const hu_reaction_collection_config_t *cfg) {
    if (!cfg || !cfg->enabled)
        return false;
    if (cfg->channel_count == 0)
        return true;
    for (size_t i = 0; i < cfg->channel_count; i++) {
        if (strcmp(cfg->channels[i], "imessage") == 0)
            return true;
    }
    return false;
}

static const char *resolve_chatdb_path(const hu_reaction_collection_config_t *cfg) {
    if (cfg && cfg->chatdb_path[0])
        return cfg->chatdb_path;
    const char *env = getenv("HU_CHATDB");
    if (env && env[0])
        return env;
    static char home_path[512];
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    snprintf(home_path, sizeof(home_path), "%s/Library/Messages/chat.db", home);
    return home_path;
}

static void free_reaction_events(hu_reaction_event_t *events, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free((void *)events[i].target_thread_id);
        free((void *)events[i].target_message_ref);
        free((void *)events[i].sender_handle);
    }
}

hu_error_t hu_daemon_tick_reaction_poll(const hu_reaction_collection_config_t *cfg,
                                        int64_t now_unix, int64_t *last_poll_unix_inout,
                                        int64_t *watermark_inout) {
    if (!cfg || !last_poll_unix_inout || !watermark_inout)
        return HU_ERR_INVALID_ARGUMENT;
    if (!reaction_collection_wants_imessage(cfg))
        return HU_OK;

    int interval = cfg->poll_interval_seconds > 0 ? cfg->poll_interval_seconds : 30;
    if (*last_poll_unix_inout > 0 && now_unix - *last_poll_unix_inout < interval)
        return HU_OK;

    const char *db = resolve_chatdb_path(cfg);
    if (!db || !db[0])
        return HU_OK;

    hu_reaction_event_t events[8];
    size_t n = 0;
    int64_t since = *watermark_inout > 0 ? *watermark_inout : now_unix;
    hu_error_t err = hu_imessage_poll_reactions(db, since, events, 8, &n);

#if HU_IS_TEST
    g_poll_count_for_test++;
    if (g_imessage_poll_call_counter)
        (*g_imessage_poll_call_counter)++;
#endif

    *last_poll_unix_inout = now_unix;
    *watermark_inout = now_unix;

    if (err == HU_OK) {
        for (size_t i = 0; i < n; i++)
            (void)hu_reaction_handler_handle_event(&events[i]);
    }
    free_reaction_events(events, n);

    return err == HU_ERR_IO ? err : HU_OK;
}

#if HU_IS_TEST
hu_error_t hu_daemon_tick_for_test(const hu_config_t *cfg) {
    if (!cfg)
        return HU_ERR_INVALID_ARGUMENT;
    static int64_t last_poll = 0;
    static int64_t watermark = 0;
    int64_t now = 1000;
    if (last_poll > 0)
        now = last_poll + (int64_t)cfg->reaction_collection.poll_interval_seconds;
    return hu_daemon_tick_reaction_poll(&cfg->reaction_collection, now, &last_poll, &watermark);
}
#endif
